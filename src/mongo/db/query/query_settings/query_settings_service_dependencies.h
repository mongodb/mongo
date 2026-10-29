// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/util/modules.h"

namespace mongo::query_settings {
using SetClusterParameterFn = std::function<void(OperationContext*,
                                                 const SetClusterParameter&,
                                                 boost::optional<Timestamp>,
                                                 boost::optional<LogicalTime>)>;

/**
 * Dependencies needed for QuerySettingsService initialization.
 * The dependencies are provided by `ServiceContext::ConstructorActionRegisterer`
 * nodes that are specified to execute before "QuerySettingsService".
 * This abstraction mechanism breaks cyclic dependecies.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] ServiceDependencies {
    SetClusterParameterFn setClusterParameterReplSet;
    SetClusterParameterFn setClusterParameterConfigsvr;
};

[[MONGO_MOD_NEEDS_REPLACEMENT]]
ServiceDependencies& getServiceDependencies(ServiceContext* serviceContext);

}  // namespace mongo::query_settings
