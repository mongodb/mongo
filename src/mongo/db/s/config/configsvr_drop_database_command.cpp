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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

/**
 * Internal sharding command run on config servers to drop a database.
 */
class ConfigSvrDropDatabaseCommand : public BasicCommand {
public:
    ConfigSvrDropDatabaseCommand() : BasicCommand("_configsvrDropDatabase") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Drops a database.";
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

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return cmdObj.firstElement().str();
    }


    bool run(OperationContext* opCtx,
             const std::string& dbname_unused,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            uasserted(ErrorCodes::IllegalOperation,
                      "_configsvrDropDatabase can only be run on config servers");
        }

        const std::string dbname = parseNs("", cmdObj);

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "dropDatabase must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        auto const catalogClient = Grid::get(opCtx)->catalogClient();
        auto const catalogManager = ShardingCatalogManager::get(opCtx);

        auto dbDistLock = uassertStatusOK(catalogClient->getDistLockManager()->lock(
            opCtx, dbname, "dropDatabase", DistLockManager::kDefaultLockTimeout));

        // Invalidate the database metadata so the next access kicks off a full reload.
        ON_BLOCK_EXIT([opCtx, dbname] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbname); });

        auto dbInfo =
            catalogClient->getDatabase(opCtx, dbname, repl::ReadConcernLevel::kLocalReadConcern);

        // If the namespace isn't found, treat the drop as a success. In case the drop just happened
        // and has not fully propagated, set the client's last optime to the system's last optime to
        // ensure the client waits.
        if (dbInfo.getStatus() == ErrorCodes::NamespaceNotFound) {
            result.append("info", "database does not exist");
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            return true;
        }

        // If we didn't get a NamespaceNotFound error, make sure there wasn't some other type of
        // error.
        auto dbType = uassertStatusOK(dbInfo).value;

        catalogClient
            ->logChange(opCtx,
                        "dropDatabase.start",
                        dbname,
                        BSONObj(),
                        ShardingCatalogClient::kMajorityWriteConcern)
            .transitional_ignore();

        // Drop the database's collections.
        for (const auto& nss : catalogClient->getAllShardedCollectionsForDb(
                 opCtx, dbname, repl::ReadConcernLevel::kLocalReadConcern)) {
            auto collDistLock = uassertStatusOK(catalogClient->getDistLockManager()->lock(
                opCtx, nss.ns(), "dropCollection", DistLockManager::kDefaultLockTimeout));
            uassertStatusOK(catalogManager->dropCollection(opCtx, nss));
        }

        // Drop the database from the primary shard first.
        _dropDatabaseFromShard(opCtx, dbType.getPrimary(), dbname);

        // Drop the database from each of the remaining shards.
        {
            std::vector<ShardId> allShardIds;
            Grid::get(opCtx)->shardRegistry()->getAllShardIdsNoReload(&allShardIds);

            for (const ShardId& shardId : allShardIds) {
                _dropDatabaseFromShard(opCtx, shardId, dbname);
            }
        }

        // Remove the database entry from the metadata.
        Status status =
            catalogClient->removeConfigDocuments(opCtx,
                                                 DatabaseType::ConfigNS,
                                                 BSON(DatabaseType::name(dbname)),
                                                 ShardingCatalogClient::kMajorityWriteConcern);
        uassertStatusOKWithContext(
            status, str::stream() << "Could not remove database '" << dbname << "' from metadata");

        catalogClient
            ->logChange(opCtx,
                        "dropDatabase",
                        dbname,
                        BSONObj(),
                        ShardingCatalogClient::kMajorityWriteConcern)
            .transitional_ignore();

        result.append("dropped", dbname);
        return true;
    }

private:
    /**
     * Sends the 'dropDatabase' command for the specified database to the specified shard. Throws
     * DBException on failure.
     */
    static void _dropDatabaseFromShard(OperationContext* opCtx,
                                       const ShardId& shardId,
                                       const std::string& dbName) {

        const auto dropDatabaseCommandBSON = [opCtx] {
            BSONObjBuilder builder;
            builder.append("dropDatabase", 1);
            builder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
            return builder.obj();
        }();

        const auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
        auto cmdDropDatabaseResult = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            dbName,
            dropDatabaseCommandBSON,
            Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(cmdDropDatabaseResult.commandStatus);
        uassertStatusOK(cmdDropDatabaseResult.writeConcernStatus);
    };

} configsvrDropDatabaseCmd;

}  // namespace
}  // namespace mongo
