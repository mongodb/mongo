/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/server_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

bool isClusterNode() {
    return serverGlobalParams.clusterRole != ClusterRole::None;
}

class ShardingServerStatus final : public ServerStatusSection {
public:
    ShardingServerStatus() : ServerStatusSection("sharding") {}

    bool includeByDefault() const override {
        return isClusterNode();
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!isClusterNode())
            return {};

        auto const shardingState = ShardingState::get(opCtx);
        if (!shardingState->enabled())
            return {};

        auto const grid = Grid::get(opCtx);
        auto const shardRegistry = grid->shardRegistry();

        BSONObjBuilder result;

        result.append("configsvrConnectionString",
                      shardRegistry->getConfigServerConnectionString().toString());

        grid->configOpTime().append(&result, "lastSeenConfigServerOpTime");

        const long long maxChunkSizeInBytes =
            grid->getBalancerConfiguration()->getMaxChunkSizeBytes();
        result.append("maxChunkSizeInBytes", maxChunkSizeInBytes);

        // Get a migration status report if a migration is active for which this is the source
        // shard. ShardingState::getActiveMigrationStatusReport will take an IS lock on the
        // namespace of the active migration if there is one that is active.
        BSONObj migrationStatus = shardingState->getActiveMigrationStatusReport(opCtx);
        if (!migrationStatus.isEmpty()) {
            result.append("migrations", migrationStatus);
        }

        return result.obj();
    }

} shardingServerStatus;

class ShardingStatisticsServerStatus final : public ServerStatusSection {
public:
    ShardingStatisticsServerStatus() : ServerStatusSection("shardingStatistics") {}

    bool includeByDefault() const override {
        return isClusterNode();
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!isClusterNode())
            return {};

        auto const shardingState = ShardingState::get(opCtx);
        if (!shardingState->enabled())
            return {};

        auto const grid = Grid::get(opCtx);
        auto const catalogCache = grid->catalogCache();

        BSONObjBuilder result;
        ShardingStatistics::get(opCtx).report(&result);
        catalogCache->report(&result);
        return result.obj();
    }

} shardingStatisticsServerStatus;

}  // namespace
}  // namespace mongo
