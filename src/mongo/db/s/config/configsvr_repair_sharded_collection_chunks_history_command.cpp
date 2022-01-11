/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class ConfigSvrRepairShardedCollectionChunksHistoryCommand : public BasicCommand {
public:
    ConfigSvrRepairShardedCollectionChunksHistoryCommand()
        : BasicCommand("_configsvrRepairShardedCollectionChunksHistory") {}

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly.";
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

    std::string parseNs(const std::string& unusedDbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
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
             const std::string& unusedDbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrRepairShardedCollectionChunksHistory can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "_configsvrRepairShardedCollectionChunksHistory must be called "
                                 "with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        const NamespaceString nss{parseNs(unusedDbName, cmdObj)};

        auto currentTime = VectorClock::get(opCtx)->getTime();
        auto validAfter = currentTime.configTime().asTimestamp();

        ShardingCatalogManager::get(opCtx)->upgradeChunksHistory(
            opCtx, nss, cmdObj["force"].booleanSafe(), validAfter);

        Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);

        return true;
    }

} configSvrRepairShardedCollectionChunksHistoryCommand;

}  // namespace
}  // namespace mongo
