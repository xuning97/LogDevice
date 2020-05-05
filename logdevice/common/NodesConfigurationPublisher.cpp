/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logdevice/common/NodesConfigurationPublisher.h"

#include "logdevice/common/configuration/nodes/NodesConfigurationCodec.h"
#include "logdevice/common/configuration/nodes/NodesConfigurationTracer.h"

namespace facebook { namespace logdevice {

NodesConfigurationPublisher::NodesConfigurationPublisher(
    std::shared_ptr<UpdateableConfig> config,
    UpdateableSettings<Settings> settings,
    std::shared_ptr<TraceLogger> trace_logger,
    bool subscribe)
    : config_(std::move(config)),
      settings_(std::move(settings)),
      tracer_(std::move(trace_logger)) {
  ld_check(config_ != nullptr);

  if (subscribe) {
    // It's ok to bind to `this` in here as the subscriptions are destroyed
    // before the destruction of this class.
    settings_subscription_ = settings_.subscribeToUpdates(
        std::bind(&NodesConfigurationPublisher::publish, this));
    server_config_subscription_ =
        config_->updateableServerConfig()->subscribeToUpdates(
            std::bind(&NodesConfigurationPublisher::publish, this));
    ncm_nodes_configuration_subscription_ =
        config_->updateableNCMNodesConfiguration()->subscribeToUpdates(
            std::bind(&NodesConfigurationPublisher::publish, this));
  }

  // Do the inital publishing
  publish();
}

void NodesConfigurationPublisher::publish() {
  // Different subscriptions can call publish() from different threads.
  std::lock_guard lock(mutex_);

  auto settings = settings_.get();
  bool from_ncm = settings->enable_nodes_configuration_manager &&
      settings->use_nodes_configuration_manager_nodes_configuration;

  auto ncm_nc = config_->getNodesConfigurationFromNCMSource();
  auto server_config_nc =
      config_->getNodesConfigurationFromServerConfigSource();
  auto nodes_configuration_to_publish = from_ncm ? ncm_nc : server_config_nc;

  ld_check(nodes_configuration_to_publish != nullptr);

  // Only publish the config if it's not equal to the existing one
  auto current_nodes_configuration = config_->getNodesConfiguration();
  if (current_nodes_configuration != nodes_configuration_to_publish) {
    int rv = config_->updateableNodesConfiguration()->update(
        nodes_configuration_to_publish);
    if (rv != 0) {
      ld_error("Failed to publish NodesConfiguration with version %ld: %s",
               nodes_configuration_to_publish->getVersion().val(),
               error_description(err));
      // TODO: log sample to an error dataset
    } else {
      ld_info("Published a NodesConfiguration with version %ld from %s",
              nodes_configuration_to_publish->getVersion().val(),
              from_ncm ? "NodesConfigurationManager" : "ServerConfig");
    }
  }
}
}} // namespace facebook::logdevice
