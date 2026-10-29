// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/periodic_sharded_index_consistency_checker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/sharding_config_server_parameters_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/platform/atomic.h"

#include <memory>

namespace mongo {
namespace {

bool isConfigServerWithShardedIndexConsistencyCheckEnabled() {
    return serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
        enableShardedIndexConsistencyCheck.load();
}

class ShardedIndexConsistencyServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return isConfigServerWithShardedIndexConsistencyCheckEnabled();
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!isConfigServerWithShardedIndexConsistencyCheckEnabled()) {
            return {};
        }

        BSONObjBuilder builder;
        builder.append("numShardedCollectionsWithInconsistentIndexes",
                       PeriodicShardedIndexConsistencyChecker::get(opCtx->getServiceContext())
                           .getNumShardedCollsWithInconsistentIndexes());
        return builder.obj();
    }
};
auto& indexConsistencyServerStatus =
    *ServerStatusSectionBuilder<ShardedIndexConsistencyServerStatus>("shardedIndexConsistency")
         .forShard();

}  // namespace
}  // namespace mongo
