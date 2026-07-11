// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log_util.h"

namespace mongo {

MONGO_INITIALIZER_GENERAL(SetShouldEmitLogService, ("EndServerParameterRegistration"), ())
(InitializerContext*) {
    logv2::setShouldEmitLogService(
        []() { return !serverGlobalParams.clusterRole.has(ClusterRole::None); });
}

}  // namespace mongo
