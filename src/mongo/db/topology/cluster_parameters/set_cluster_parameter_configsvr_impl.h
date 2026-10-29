// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

void setClusterParameterImplConfigsvr(OperationContext* opCtx,
                                      const SetClusterParameter& request,
                                      boost::optional<Timestamp>,
                                      boost::optional<LogicalTime> previousTime);

void _configsvrSetClusterParameterCmdHandler(OperationContext* opCtx,
                                             const ConfigsvrSetClusterParameter& request);

}  // namespace mongo
