/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "logdevice/admin/Conv.h"
#include "logdevice/admin/safety/SafetyChecker.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/lib/ClientImpl.h"
#include "logdevice/lib/ops/EventLogUtils.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"

using namespace ::testing;
using namespace facebook::logdevice;

// There are at least three approaches to test drains / modify node sets
// a) varying size of the cluster and rely on SelectAllNodeSetSelector
// b) use TestNodeSetSelector
// c) use NodeSetTest.CHANGE_NODESET

// (a) is used as direct modification of nodesets from outside of the cluster
// potentially may be disabled in future due to AutoLogProvisioning

const logid_t LOG_ID{1};
const logid_t LOG_ID2{2};

class SafetyAPIIntegrationTest : public IntegrationTestBase {
 protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
  }
};

namespace {
void write_test_records(std::shared_ptr<Client> client,
                        logid_t logid,
                        size_t num_records) {
  static size_t counter = 0;
  for (size_t i = 0; i < num_records; ++i) {
    std::string data("data" + std::to_string(++counter));
    lsn_t lsn = client->appendSync(logid, Payload(data.data(), data.size()));
    ASSERT_NE(LSN_INVALID, lsn)
        << "Append failed (E::" << error_name(err) << ")";
  }
}
} // namespace

logsconfig::LogAttributes createInternalLogAttributes() {
  return logsconfig::LogAttributes()
      .with_singleWriter(false)
      .with_replicationFactor(3)
      .with_syncedCopies(0);
}

TEST_F(SafetyAPIIntegrationTest, DrainWithExpand) {
  const size_t num_nodes = 3;
  const size_t num_shards = 2;

  auto nodes_configuration =
      createSimpleNodesConfig(num_nodes, num_shards, false, 2);

  // Promote N0 and N2 to become metadata nodes
  nodes_configuration = nodes_configuration->applyUpdate(
      NodesConfigurationTestUtil::setStorageMembershipUpdate(
          *nodes_configuration,
          {ShardID(0, -1), ShardID(2, -1)},
          folly::none,
          membership::MetaDataStorageState::METADATA));

  auto log_attrs = logsconfig::LogAttributes().with_replicationFactor(2);

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     .setNodes(std::move(nodes_configuration))
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogGroupName("test_range")
                     .setLogAttributes(log_attrs)
                     .create(num_nodes);

  cluster->waitUntilAllStartedAndPropagatedInGossip();

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  ld_info("LogsConfig Version: %lu",
          cluster->getConfig()->getLogsConfig()->getVersion());
  SafetyChecker safety_checker(&client_impl->getProcessor());
  ShardSet shards;

  for (int i = 0; i < num_nodes; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  // it is unsafe to drain all shards
  folly::Expected<Impact, Status> impact =
      // block until completion for tests.
      safety_checker
          .checkImpact(shard_status,
                       shards,
                       {},
                       configuration::StorageState::READ_ONLY,
                       SafetyMargin(),
                       /* check_metadata_logs = */ true,
                       /* check_internal_logs = */ true,
                       /* check_capacity = */ true,
                       /* max_unavailable_storage_capacity_pct = */ 100,
                       /* max_unavailable_sequencing_capacity_pct = */ 100)
          .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(1, impact->impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::WRITE_AVAILABILITY_LOSS,
            impact->impact_ref()->at(0));

  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));
  ASSERT_TRUE(impact->logs_affected_ref().has_value());
  ASSERT_GE(impact->logs_affected_ref()->size(), 1);
  auto impact_on_epoch = impact->logs_affected_ref()->at(0);
  ASSERT_EQ(1, impact_on_epoch.impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::WRITE_AVAILABILITY_LOSS,
            impact_on_epoch.impact_ref()->at(0));

  // Metadata logs
  ASSERT_EQ(0, *impact_on_epoch.log_id_ref());
  ASSERT_EQ(0, *impact_on_epoch.epoch_ref());
  ASSERT_EQ(
      ReplicationProperty({{NodeLocationScope::NODE, 2}}),
      toLogDevice<ReplicationProperty>(*impact_on_epoch.replication_ref()));

  // we have replication factor 2, NodeSet includes all nodes
  // it is safe to drain 1 node
  shards.clear();
  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(1, i));
  }

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::READ_ONLY,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // draining 2 nodes is unsafe as we will would have not enough nodes
  // to replicate
  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(2, i));
  }

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::READ_ONLY,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(1, impact->impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::WRITE_AVAILABILITY_LOSS,
            impact->impact_ref()->at(0));
  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));

  // double cluster size
  cluster->expand(num_nodes);
  cluster->waitUntilAllStartedAndPropagatedInGossip();
  cluster->waitUntilAllSequencersQuiescent();

  write_test_records(client, LOG_ID, 10);

  shards.clear();
  for (int i = 0; i < num_nodes; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  // try to shrink first num_nodes nodes
  // this is going to cause write stall as metadat nodes are only on first nodes
  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::READ_ONLY,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(1, impact->impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::WRITE_AVAILABILITY_LOSS,
            impact->impact_ref()->at(0));
  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));
}

TEST_F(SafetyAPIIntegrationTest, DrainWithSetWeight) {
  const size_t num_nodes = 5;
  const size_t num_shards = 2;

  auto nodes_configuration =
      createSimpleNodesConfig(num_nodes, num_shards, true, 3);

  auto log_attrs = logsconfig::LogAttributes().with_replicationFactor(2);

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     .setNodes(std::move(nodes_configuration))
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogGroupName("test_range")
                     .setLogAttributes(log_attrs)
                     .create(num_nodes);

  cluster->waitUntilAllStartedAndPropagatedInGossip();

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  SafetyChecker safety_checker(&client_impl->getProcessor());
  safety_checker.setAbortOnError(false);
  ShardSet shards;

  for (int i = 0; i < 2; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // it is safe to drain 2 nodes as nodeset size is 5, replication is 2
  folly::Expected<Impact, Status> impact =
      safety_checker
          .checkImpact(shard_status,
                       shards,
                       {},
                       configuration::StorageState::READ_ONLY,
                       SafetyMargin(),
                       /* check_metadata_logs = */ true,
                       /* check_internal_logs = */ true,
                       /* check_capacity = */ true,
                       /* max_unavailable_storage_capacity_pct = */ 100,
                       /* max_unavailable_sequencing_capacity_pct = */ 100)
          .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // make nodes read only
  cluster->updateNodeAttributes(2, configuration::StorageState::READ_ONLY, 1);
  cluster->updateNodeAttributes(3, configuration::StorageState::READ_ONLY, 1);
  cluster->updateNodeAttributes(4, configuration::StorageState::READ_ONLY, 1);
  cluster->waitForMetaDataLogWrites();

  // now it is unsafe to drain first 2 nodes
  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::READ_ONLY,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(2, impact->impact_ref()->size());
  ASSERT_THAT(
      *impact->impact_ref(),
      UnorderedElementsAre(thrift::OperationImpact::REBUILDING_STALL,
                           thrift::OperationImpact::WRITE_AVAILABILITY_LOSS));
  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));
}

TEST_F(SafetyAPIIntegrationTest, DrainWithEventLogNotReadable) {
  const size_t num_nodes = 5;
  const size_t num_shards = 2;

  auto log_attrs = logsconfig::LogAttributes().with_replicationFactor(2);

  const auto internal_log_attrs = createInternalLogAttributes();

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogGroupName("test_range")
                     .setLogAttributes(log_attrs)
                     .setEventLogAttributes(internal_log_attrs)
                     .create(num_nodes);

  cluster->waitUntilAllStartedAndPropagatedInGossip();

  std::shared_ptr<Client> client =
      cluster->createClient(std::chrono::seconds(10));
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  SafetyChecker safety_checker(&client_impl->getProcessor());
  safety_checker.setAbortOnError(false);
  ShardSet shards;

  for (int i = 0; i < 3; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // it is unsafe to drain 3 nodes as replication is 3 for event log
  folly::Expected<Impact, Status> impact =
      safety_checker
          .checkImpact(shard_status,
                       shards,
                       {},
                       configuration::StorageState::READ_ONLY,
                       SafetyMargin(),
                       /* check_metadata_logs = */ true,
                       /* check_internal_logs = */ true,
                       /* check_capacity = */ true,
                       /* max_unavailable_storage_capacity_pct = */ 100,
                       /* max_unavailable_sequencing_capacity_pct = */ 100)
          .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(2, impact->impact_ref()->size());
  ASSERT_THAT(
      *impact->impact_ref(),
      UnorderedElementsAre(thrift::OperationImpact::REBUILDING_STALL,
                           thrift::OperationImpact::WRITE_AVAILABILITY_LOSS));
  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));

  // with event log replication factor 3, it is fine to loose two node
  cluster->getNode(num_nodes - 1).suspend();
  cluster->getNode(num_nodes - 2).suspend();

  shards.clear();
  shards.insert(ShardID(3, 0));

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::READ_ONLY,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());
}

TEST_F(SafetyAPIIntegrationTest, DisableReads) {
  const size_t num_nodes = 5;
  const size_t num_shards = 3;

  auto nodes_configuration =
      createSimpleNodesConfig(num_nodes, num_shards, true, 3);

  auto log_attrs = logsconfig::LogAttributes().with_replicationFactor(3);

  const auto internal_log_attrs = createInternalLogAttributes();

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(2)
                     .setNodes(std::move(nodes_configuration))
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogGroupName("test_range")
                     .setLogAttributes(log_attrs)
                     .setEventLogAttributes(internal_log_attrs)
                     .setConfigLogAttributes(internal_log_attrs)
                     .create(num_nodes);

  cluster->waitUntilAllStartedAndPropagatedInGossip();

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);
  write_test_records(client, LOG_ID2, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  SafetyChecker safety_checker(&client_impl->getProcessor());
  // Get all possible errors.
  safety_checker.setAbortOnError(false);
  ShardSet shards;

  for (int i = 0; i < num_nodes; ++i) {
    shards.insert(ShardID(i, 0));
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // it is unsafe to stop all shards
  folly::Expected<Impact, Status> impact =
      safety_checker
          .checkImpact(shard_status,
                       shards,
                       {},
                       configuration::StorageState::DISABLED,
                       SafetyMargin(),
                       /* check_metadata_logs = */ true,
                       /* check_internal_logs = */ true,
                       /* check_capacity = */ true,
                       /* max_unavailable_storage_capacity_pct = */ 100,
                       /* max_unavailable_sequencing_capacity_pct = */ 100)
          .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(3, impact->impact_ref()->size());
  ASSERT_THAT(
      *impact->impact_ref(),
      UnorderedElementsAre(thrift::OperationImpact::READ_AVAILABILITY_LOSS,
                           thrift::OperationImpact::WRITE_AVAILABILITY_LOSS,
                           thrift::OperationImpact::REBUILDING_STALL));
  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));

  // we have replication factor 3, NodeSet includes all nodes
  // it is safe to stop 2 node
  shards.clear();
  for (int i = 0; i < 2; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // stoping 3 same shards is unsafe
  shards.clear();
  for (int i = 0; i < 3; ++i) {
    shards.insert(ShardID(i, 2));
  }

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(3, impact->impact_ref()->size());
  ASSERT_THAT(
      *impact->impact_ref(),
      UnorderedElementsAre(thrift::OperationImpact::READ_AVAILABILITY_LOSS,
                           thrift::OperationImpact::WRITE_AVAILABILITY_LOSS,
                           thrift::OperationImpact::REBUILDING_STALL));

  // stoping 3 different shards is fine
  shards.clear();
  shards.insert(ShardID(1, 1));
  shards.insert(ShardID(2, 2));
  shards.insert(ShardID(3, 3));
  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());
  // Check that we don't set this on empty OperationImpact
  ASSERT_TRUE(impact->internal_logs_affected_ref().has_value());
  ASSERT_FALSE(*impact->internal_logs_affected_ref());
}

TEST_F(SafetyAPIIntegrationTest, SafetyMargin) {
  const size_t num_nodes = 5;
  const size_t num_shards = 5;

  auto nodes_configuration =
      createSimpleNodesConfig(num_nodes, num_shards, true, 3);

  auto log_attrs = logsconfig::LogAttributes().with_replicationFactor(3);

  const auto internal_log_attrs = createInternalLogAttributes();

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNumLogs(1)
                     .setNodes(std::move(nodes_configuration))
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .setLogGroupName("test_range")
                     .setLogAttributes(log_attrs)
                     .setEventLogAttributes(internal_log_attrs)
                     .setConfigLogAttributes(internal_log_attrs)
                     .create(num_nodes);

  cluster->waitUntilAllStartedAndPropagatedInGossip();

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  write_test_records(client, LOG_ID, 10);

  ld_info("Waiting for metadata log writes to complete");
  cluster->waitForMetaDataLogWrites();

  // double cluster size
  // cluster->expand(num_nodes);
  // for (const auto& it : cluster->getNodes()) {
  //   node_index_t idx = it.first;
  //   cluster->getNode(idx).waitUntilAvailable();
  // }

  // nodeset size is 3, first three nodes
  SafetyChecker safety_checker(&client_impl->getProcessor());
  safety_checker.setAbortOnError(false);
  ShardSet shards;

  for (int i = 0; i < num_nodes; ++i) {
    for (int s = 0; s < num_shards; ++s) {
      shards.insert(ShardID(i, s));
    }
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // we have replication factor 3, NodeSet includes 3 nodes out of 6
  // it is safe to drain 1 node
  shards.clear();
  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(1, i));
  }

  SafetyMargin safety;

  folly::Expected<Impact, Status> impact =
      safety_checker
          .checkImpact(shard_status,
                       shards,
                       {},
                       configuration::StorageState::READ_ONLY,
                       safety,
                       /* check_metadata_logs = */ true,
                       /* check_internal_logs = */ true,
                       /* check_capacity = */ true,
                       /* max_unavailable_storage_capacity_pct = */ 100,
                       /* max_unavailable_sequencing_capacity_pct = */ 100)
          .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            safety,
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)

               .get();
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // it is safe if we want to have 1 extra node
  safety[NodeLocationScope::NODE] = 1;
  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::READ_ONLY,
                            safety,
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            safety,
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // it is unsafe if we wantto have 2 extra nodes
  safety[NodeLocationScope::NODE] = 2;
  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::READ_ONLY,
                            safety,
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(2, impact->impact_ref()->size());
  ASSERT_THAT(
      *impact->impact_ref(),
      UnorderedElementsAre(thrift::OperationImpact::REBUILDING_STALL,
                           thrift::OperationImpact::WRITE_AVAILABILITY_LOSS));
  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));

  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            safety,
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(3, impact->impact_ref()->size());
  ASSERT_THAT(
      *impact->impact_ref(),
      UnorderedElementsAre(thrift::OperationImpact::READ_AVAILABILITY_LOSS,
                           thrift::OperationImpact::REBUILDING_STALL,
                           thrift::OperationImpact::WRITE_AVAILABILITY_LOSS));
  ASSERT_TRUE(impact->internal_logs_affected_ref().value_or(false));

  for (int i = 0; i < num_shards; ++i) {
    shards.insert(ShardID(2, i));
  }

  // it is fine to drain 2 nodes, without safety maring
  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // but not enough if we want one extra node
  safety[NodeLocationScope::NODE] = 1;
  impact = safety_checker
               .checkImpact(shard_status,
                            shards,
                            {},
                            configuration::StorageState::DISABLED,
                            safety,
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(3, impact->impact_ref()->size());
  ASSERT_THAT(
      *impact->impact_ref(),
      UnorderedElementsAre(thrift::OperationImpact::READ_AVAILABILITY_LOSS,
                           thrift::OperationImpact::REBUILDING_STALL,
                           thrift::OperationImpact::WRITE_AVAILABILITY_LOSS));
}

TEST_F(SafetyAPIIntegrationTest, Capacity) {
  const size_t num_nodes = 5;
  const size_t num_shards = 3;

  Configuration::Nodes nodes;

  // Adding sequencer role to all nodes
  // Weight of 0, 1 is 1.5, sequencer on 2 is disabled.
  // Given that num_nodes = 5, total weight is 6.
  // Where N0, N1 are 50% of the total weight.
  for (int i = 0; i < num_nodes; ++i) {
    double weight = 1.0;
    if (i == 0 || i == 1) {
      // Node 0, 1 has sequencer weight of 1.5
      weight = 1.5;
    }
    nodes[i].generation = 1;
    bool sequencer_enabled = true;
    if (i == 2) {
      sequencer_enabled = false;
    }
    nodes[i].addSequencerRole(sequencer_enabled, weight);
    // Add storage role to all 5 nodes,
    // capacity of num_nodes and num_nodes - 1 is 1.5
    // Given that num_nodes = 5, total capacity is 6.
    // Where N4, N3 are 50% of the total capacity.
    // N0 is a READ_ONLY node.
    double capacity = 1.0;
    if (i == num_nodes - 2 || i == num_nodes - 1) {
      capacity = 1.5;
    }
    nodes[i].addStorageRole(num_shards, capacity);
  }

  auto nodes_configuration = NodesConfigurationTestUtil::provisionNodes(
      std::move(nodes), ReplicationProperty{{NodeLocationScope::NODE, 2}});

  // N0 is READ_ONLY.
  nodes_configuration = nodes_configuration->applyUpdate(
      NodesConfigurationTestUtil::setStorageMembershipUpdate(
          *nodes_configuration,
          {ShardID(0, -1)},
          membership::StorageState::READ_ONLY,
          folly::none));

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNodes(std::move(nodes_configuration))
                     // switches on gossip
                     .useHashBasedSequencerAssignment()
                     .setNumDBShards(num_shards)
                     .create(num_nodes);

  for (const auto& it : cluster->getNodes()) {
    node_index_t idx = it.first;
    cluster->getNode(idx).waitUntilAvailable();
  }

  std::shared_ptr<Client> client = cluster->createClient();
  ClientImpl* client_impl = dynamic_cast<ClientImpl*>(client.get());

  SafetyChecker safety_checker(&client_impl->getProcessor());
  // Get all possible errors.
  safety_checker.setAbortOnError(true);
  folly::F14FastSet<node_index_t> seqs;

  for (int i = 0; i < num_nodes; ++i) {
    seqs.insert(i);
  }

  ShardAuthoritativeStatusMap shard_status{LSN_INVALID};
  int rv = cluster->getShardAuthoritativeStatusMap(shard_status);
  ASSERT_EQ(0, rv);

  // it is unsafe to stop all sequencers, we should allow maximum 25% of the
  folly::Expected<Impact, Status> impact =
      safety_checker
          .checkImpact(shard_status,
                       {},
                       seqs,
                       configuration::StorageState::DISABLED,
                       SafetyMargin(),
                       /* check_metadata_logs = */ true,
                       /* check_internal_logs = */ true,
                       /* check_capacity = */ true,
                       /* max_unavailable_storage_capacity_pct = */ 25,
                       /* max_unavailable_sequencing_capacity_pct = */ 25)
          .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(1, impact->impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::SEQUENCING_CAPACITY_LOSS,
            impact->impact_ref()->at(0));

  // 50% of capacity via 2 nodes, that should be still fail. Because N2 is
  // disabled, we already lost 16.6% of capacity.
  impact = safety_checker
               .checkImpact(shard_status,
                            {},
                            {0, 1}, // 50% of the sequencing capacity
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 50)
               .get();

  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(1, impact->impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::SEQUENCING_CAPACITY_LOSS,
            impact->impact_ref()->at(0));

  // Pass since N1 = 25% and N2 is 16.6%, and the limit is 50%.
  impact = safety_checker
               .checkImpact(shard_status,
                            {},
                            {1}, // 25% of the sequencing capacity
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 100,
                            /* max_unavailable_sequencing_capacity_pct = */ 50)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // STORAGE CAPACITY

  // Fail because the limit is 40% and we are losing 25% in addition to the
  // disabled node (N2 = 16.6%)
  impact = safety_checker
               .checkImpact(shard_status,
                            // 25% of the total capacity
                            {ShardID(3, 0), ShardID(3, 1), ShardID(3, 2)},
                            {},
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 40,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(1, impact->impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::STORAGE_CAPACITY_LOSS,
            impact->impact_ref()->at(0));

  // Success because the limit is 50% and we are losing 25% in addition to the
  // disabled node (N2 = 16.6%)
  impact = safety_checker
               .checkImpact(shard_status,
                            // 25% of the total capacity
                            {ShardID(3, 0), ShardID(3, 1), ShardID(3, 2)},
                            {},
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 50,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(0, impact->impact_ref()->size());

  // Fail because N4:S0, N4:S1 are UNAVAILABLE/EMPTY (total capacity loss of 2/3
  // * 25% = 16.6%) the limit is 50% and we are losing 25% (N3) in addition to
  // the disabled node (N2 = 16.6%)
  shard_status.setShardStatus(4, 0, AuthoritativeStatus::UNAVAILABLE);
  shard_status.setShardStatus(4, 1, AuthoritativeStatus::AUTHORITATIVE_EMPTY);
  impact = safety_checker
               .checkImpact(shard_status,
                            // 25% of the total capacity
                            {ShardID(3, 0), ShardID(3, 1), ShardID(3, 2)},
                            {},
                            configuration::StorageState::DISABLED,
                            SafetyMargin(),
                            /* check_metadata_logs = */ true,
                            /* check_internal_logs = */ true,
                            /* check_capacity = */ true,
                            /* max_unavailable_storage_capacity_pct = */ 50,
                            /* max_unavailable_sequencing_capacity_pct = */ 100)
               .get();
  ASSERT_TRUE(impact.hasValue());
  ld_info("IMPACT: %s", impactToString(*impact).c_str());
  ASSERT_EQ(1, impact->impact_ref()->size());
  ASSERT_EQ(thrift::OperationImpact::STORAGE_CAPACITY_LOSS,
            impact->impact_ref()->at(0));
}
