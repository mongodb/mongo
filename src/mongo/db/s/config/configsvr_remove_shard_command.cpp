/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers to remove a shard from the cluster.
 */
class ConfigSvrRemoveShardCommand : public BasicCommand {
public:
    ConfigSvrRemoveShardCommand() : BasicCommand("_configsvrRemoveShard") {}

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Removes a shard from the cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrRemoveShard can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << cmdObj.firstElement().fieldName()
                              << "' must be of type string",
                cmdObj.firstElement().type() == BSONType::String);
        const std::string target = cmdObj.firstElement().str();

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "removeShard must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, ShardId(target));
        if (!shardStatus.isOK()) {
            std::string msg(str::stream() << "Could not drop shard '" << target
                                          << "' because it does not exist");
            log() << msg;
            uasserted(ErrorCodes::ShardNotFound, msg);
        }
        const auto& shard = shardStatus.getValue();

        const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);

        const auto shardDrainingStatus =
            uassertStatusOK(shardingCatalogManager->removeShard(opCtx, shard->getId()));

        std::vector<std::string> databases =
            uassertStatusOK(shardingCatalogManager->getDatabasesForShard(opCtx, shard->getId()));

        // Get BSONObj containing:
        // 1) note about moving or dropping databases in a shard
        // 2) list of databases (excluding 'local' database) that need to be moved
        const auto dbInfo = [&] {
            BSONObjBuilder dbInfoBuilder;
            dbInfoBuilder.append("note", "you need to drop or movePrimary these databases");

            BSONArrayBuilder dbs(dbInfoBuilder.subarrayStart("dbsToMove"));
            for (const auto& db : databases) {
                if (db != NamespaceString::kLocalDb) {
                    dbs.append(db);
                }
            }
            dbs.doneFast();

            return dbInfoBuilder.obj();
        }();

        // TODO: Standardize/separate how we append to the result object
        switch (shardDrainingStatus) {
            case ShardDrainingStatus::STARTED:
                result.append("msg", "draining started successfully");
                result.append("state", "started");
                result.append("shard", shard->getId().toString());
                result.appendElements(dbInfo);
                break;
            case ShardDrainingStatus::ONGOING: {
                const auto swChunks = Grid::get(opCtx)->catalogClient()->getChunks(
                    opCtx,
                    BSON(ChunkType::shard(shard->getId().toString())),
                    BSONObj(),
                    boost::none,  // return all
                    nullptr,
                    repl::ReadConcernLevel::kMajorityReadConcern);
                uassertStatusOK(swChunks.getStatus());

                const auto& chunks = swChunks.getValue();
                result.append("msg", "draining ongoing");
                result.append("state", "ongoing");
                result.append("remaining",
                              BSON("chunks" << static_cast<long long>(chunks.size()) << "dbs"
                                            << static_cast<long long>(databases.size())));
                result.appendElements(dbInfo);
                break;
            }
            case ShardDrainingStatus::COMPLETED:
                result.append("msg", "removeshard completed successfully");
                result.append("state", "completed");
                result.append("shard", shard->getId().toString());
                break;
        }

        return true;
    }

} configsvrRemoveShardCmd;

}  // namespace
}  // namespace mongo
