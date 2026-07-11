// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
void setClusterParameterImplReplicaSetOrStandalone(OperationContext* opCtx,
                                                   const SetClusterParameter& request,
                                                   boost::optional<Timestamp>,
                                                   boost::optional<LogicalTime> previousTime);
}  // namespace mongo
