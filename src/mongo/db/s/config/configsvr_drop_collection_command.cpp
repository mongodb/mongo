/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(setDropCollDistLockWait);

/**
 * Internal sharding command run on config servers to drop a collection from a database.
 */
class ConfigSvrDropCollectionCommand : public BasicCommand {
public:
    ConfigSvrDropCollectionCommand() : BasicCommand("_configsvrDropCollection") {}

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
               "directly. Drops a collection from a database.";
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

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrDropCollection can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        const NamespaceString nss(parseNs(dbname, cmdObj));

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "dropCollection must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        Seconds waitFor(DistLockManager::kDefaultLockTimeout);
        setDropCollDistLockWait.execute(
            [&](const BSONObj& data) { waitFor = Seconds(data["waitForSecs"].numberInt()); });

        auto const catalogClient = Grid::get(opCtx)->catalogClient();

        auto scopedDbLock =
            ShardingCatalogManager::get(opCtx)->serializeCreateOrDropDatabase(opCtx, nss.db());
        auto scopedCollLock =
            ShardingCatalogManager::get(opCtx)->serializeCreateOrDropCollection(opCtx, nss);

        auto dbDistLock = uassertStatusOK(
            catalogClient->getDistLockManager()->lock(opCtx, nss.db(), "dropCollection", waitFor));
        auto collDistLock = uassertStatusOK(
            catalogClient->getDistLockManager()->lock(opCtx, nss.ns(), "dropCollection", waitFor));

        ON_BLOCK_EXIT(
            [opCtx, nss] { Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss); });

        _dropCollection(opCtx, nss);

        return true;
    }

private:
    static void _dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
        auto const catalogClient = Grid::get(opCtx)->catalogClient();

        auto collStatus =
            catalogClient->getCollection(opCtx, nss, repl::ReadConcernArgs::get(opCtx).getLevel());
        if (collStatus == ErrorCodes::NamespaceNotFound) {
            // We checked the sharding catalog and found that this collection doesn't exist. This
            // may be because it never existed, or because a drop command was sent previously. This
            // data might not be majority committed though, so we will set the client's last optime
            // to the system's last optime to ensure the client waits for the writeConcern to be
            // satisfied.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

            // If the DB isn't in the sharding catalog either, consider the drop a success.
            auto dbStatus = catalogClient->getDatabase(
                opCtx, nss.db().toString(), repl::ReadConcernArgs::get(opCtx).getLevel());
            if (dbStatus == ErrorCodes::NamespaceNotFound) {
                return;
            }
            uassertStatusOK(dbStatus);

            // If we found the DB but not the collection, and the primary shard for the database is
            // the config server, run the drop only against the config server unless the collection
            // is config.system.sessions, since no other collections whose primary shard is the
            // config server can have been sharded.
            const auto primaryShard = dbStatus.getValue().value.getPrimary();
            if (primaryShard == "config" && nss != NamespaceString::kLogicalSessionsNamespace) {
                auto cmdDropResult =
                    uassertStatusOK(Grid::get(opCtx)
                                        ->shardRegistry()
                                        ->getConfigShard()
                                        ->runCommandWithFixedRetryAttempts(
                                            opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            nss.db().toString(),
                                            BSON("drop" << nss.coll()),
                                            Shard::RetryPolicy::kIdempotent));

                // If the collection doesn't exist, consider the drop a success.
                if (cmdDropResult.commandStatus == ErrorCodes::NamespaceNotFound) {
                    return;
                }
                uassertStatusOK(cmdDropResult.commandStatus);
                return;
            }

            ShardingCatalogManager::get(opCtx)->ensureDropCollectionCompleted(opCtx, nss);
        } else {
            uassertStatusOK(collStatus);
            ShardingCatalogManager::get(opCtx)->dropCollection(opCtx, nss);
        }
    }

} configsvrDropCollectionCmd;

}  // namespace
}  // namespace mongo
