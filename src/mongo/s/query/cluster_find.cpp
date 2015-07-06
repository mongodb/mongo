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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_find.h"

#include <set>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor.h"

namespace mongo {

StatusWith<CursorId> ClusterFind::runQuery(OperationContext* txn,
                                           const CanonicalQuery& query,
                                           const ReadPreferenceSetting& readPref,
                                           std::vector<BSONObj>* results) {
    invariant(results);

    auto dbConfig = grid.catalogCache()->getDatabase(query.nss().db().toString());
    if (!dbConfig.isOK()) {
        return dbConfig.getStatus();
    }

    auto shardRegistry = grid.shardRegistry();

    // Get the set of shards on which we will run the query.
    std::vector<std::shared_ptr<Shard>> shards;
    std::shared_ptr<ChunkManager> manager;
    std::shared_ptr<Shard> primary;
    dbConfig.getValue()->getChunkManagerOrPrimary(query.nss().ns(), manager, primary);
    if (primary) {
        shards.emplace_back(std::move(primary));
    } else {
        invariant(manager);

        std::set<ShardId> shardIds;
        manager->getShardIdsForQuery(shardIds, query.getParsed().getFilter());

        for (auto id : shardIds) {
            shards.emplace_back(shardRegistry->getShard(id));
        }
    }

    // Use read pref to target a particular host from each shard.
    std::vector<HostAndPort> remotes;
    for (const auto& shard : shards) {
        auto targeter = shard->getTargeter();
        auto hostAndPort = targeter->findHost(readPref);
        if (!hostAndPort.isOK()) {
            return hostAndPort.getStatus();
        }
        remotes.emplace_back(std::move(hostAndPort.getValue()));
    }

    // TODO: handle other query options (skip, limit, projection).
    ClusterClientCursorParams params(query.nss());
    params.cmdObj = query.getParsed().asFindCommand();
    params.sort = query.getParsed().getSort();

    ClusterClientCursor ccc(shardRegistry->getExecutor(), params, remotes);

    // TODO: this should implement the batching logic rather than fully exhausting the cursor. It
    // should allocate a cursor id and save the ClusterClientCursor rather than always returning a
    // cursor id of 0.
    StatusWith<boost::optional<BSONObj>> nextObj(boost::none);
    while ((nextObj = ccc.next()).isOK()) {
        if (!nextObj.getValue()) {
            break;
        }
        results->emplace_back(std::move(*nextObj.getValue()));
    }

    if (!nextObj.isOK()) {
        return nextObj.getStatus();
    }

    return CursorId(0);
}

}  // namespace mongo
