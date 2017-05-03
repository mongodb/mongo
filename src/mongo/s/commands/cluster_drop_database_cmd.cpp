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
#include "mongo/util/log.h"

namespace mongo {
namespace {

class DropDatabaseCmd : public Command {
public:
    DropDatabaseCmd() : Command("dropDatabase") {}

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
        actions.addAction(ActionType::dropDatabase);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             BSONObj& cmdObj,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "Cannot drop the config database",
                dbname != NamespaceString::kConfigDb);

        uassert(ErrorCodes::BadValue,
                "have to pass 1 as db parameter",
                cmdObj.firstElement().isNumber() && cmdObj.firstElement().number() == 1);

        auto const catalogClient = Grid::get(opCtx)->catalogClient(opCtx);

        // Lock the database globally to prevent conflicts with simultaneous database
        // creation/modification.
        auto scopedDistLock = uassertStatusOK(catalogClient->getDistLockManager()->lock(
            opCtx, dbname, "dropDatabase", DistLockManager::kDefaultLockTimeout));

        auto const catalogCache = Grid::get(opCtx)->catalogCache();

        // Refresh the database metadata so it kicks off a full reload
        catalogCache->purgeDatabase(dbname);

        auto dbInfoStatus = catalogCache->getDatabase(opCtx, dbname);

        if (dbInfoStatus == ErrorCodes::NamespaceNotFound) {
            result.append("info", "database does not exist");
            return true;
        }

        uassertStatusOK(dbInfoStatus.getStatus());

        catalogClient->logChange(opCtx,
                                 "dropDatabase.start",
                                 dbname,
                                 BSONObj(),
                                 ShardingCatalogClient::kMajorityWriteConcern);

        auto& dbInfo = dbInfoStatus.getValue();

        // Drop the database's collections from metadata
        for (const auto& nss : getAllShardedCollectionsForDb(opCtx, dbname)) {
            uassertStatusOK(catalogClient->dropCollection(opCtx, nss));
        }

        // Drop the database from the primary shard first
        _dropDatabaseFromShard(opCtx, dbInfo.primaryId(), dbname);

        // Drop the database from each of the remaining shards
        {
            std::vector<ShardId> allShardIds;
            Grid::get(opCtx)->shardRegistry()->getAllShardIds(&allShardIds);

            for (const ShardId& shardId : allShardIds) {
                _dropDatabaseFromShard(opCtx, shardId, dbname);
            }
        }

        // Remove the database entry from the metadata
        Status status =
            catalogClient->removeConfigDocuments(opCtx,
                                                 DatabaseType::ConfigNS,
                                                 BSON(DatabaseType::name(dbname)),
                                                 ShardingCatalogClient::kMajorityWriteConcern);
        if (!status.isOK()) {
            uassertStatusOK({status.code(),
                             str::stream() << "Could not remove database '" << dbname
                                           << "' from metadata due to "
                                           << status.reason()});
        }

        // Invalidate the database so the next access will do a full reload
        catalogCache->purgeDatabase(dbname);

        catalogClient->logChange(
            opCtx, "dropDatabase", dbname, BSONObj(), ShardingCatalogClient::kMajorityWriteConcern);

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
        const auto dropDatabaseCommandBSON = [opCtx, &dbName] {
            BSONObjBuilder builder;
            builder.append("dropDatabase", 1);

            if (!opCtx->getWriteConcern().usedDefault) {
                builder.append(WriteConcernOptions::kWriteConcernField,
                               opCtx->getWriteConcern().toBSON());
            }

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
    }

} clusterDropDatabaseCmd;

}  // namespace
}  // namespace mongo
