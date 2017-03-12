/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class DropCmd : public Command {
public:
    DropCmd() : Command("drop") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        ActionSet actions;
        actions.addAction(ActionType::dropCollection);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNsCollectionRequired(dbname, cmdObj));

        auto const catalogCache = Grid::get(opCtx)->catalogCache();

        auto routingInfoStatus = catalogCache->getCollectionRoutingInfo(opCtx, nss);
        if (routingInfoStatus == ErrorCodes::NamespaceNotFound) {
            return true;
        }

        auto routingInfo = uassertStatusOK(std::move(routingInfoStatus));

        if (!routingInfo.cm()) {
            _dropUnshardedCollectionFromShard(opCtx, routingInfo.primaryId(), nss, &result);
        } else {
            uassertStatusOK(Grid::get(opCtx)->catalogClient(opCtx)->dropCollection(opCtx, nss));
            catalogCache->invalidateShardedCollection(nss);
        }

        return true;
    }

private:
    /**
     * Sends the 'drop' command for the specified collection to the specified shard. Throws
     * DBException on failure.
     */
    static void _dropUnshardedCollectionFromShard(OperationContext* opCtx,
                                                  const ShardId& shardId,
                                                  const NamespaceString& nss,
                                                  BSONObjBuilder* result) {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

        const auto dropCommandBSON = [shardRegistry, opCtx, &shardId, &nss] {
            BSONObjBuilder builder;
            builder.append("drop", nss.coll());

            // Append the chunk version for the specified namespace indicating that we believe it is
            // not sharded. Collections residing on the config server are never sharded so do not
            // send the shard version.
            if (shardId != shardRegistry->getConfigShard()->getId()) {
                ChunkVersion::UNSHARDED().appendForCommands(&builder);
            }

            if (!opCtx->getWriteConcern().usedDefault) {
                builder.append(WriteConcernOptions::kWriteConcernField,
                               opCtx->getWriteConcern().toBSON());
            }

            return builder.obj();
        }();

        const auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        auto cmdDropResult = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.db().toString(),
            dropCommandBSON,
            Shard::RetryPolicy::kIdempotent));

        // Special-case SendStaleVersion errors
        if (cmdDropResult.commandStatus == ErrorCodes::SendStaleConfig) {
            throw RecvStaleConfigException(
                str::stream() << "Stale config while dropping collection", cmdDropResult.response);
        }

        uassertStatusOK(cmdDropResult.commandStatus);

        if (!cmdDropResult.writeConcernStatus.isOK()) {
            appendWriteConcernErrorToCmdResponse(
                shardId, cmdDropResult.response["writeConcernError"], *result);
        }
    }

} clusterDropCmd;

}  // namespace
}  // namespace mongo
