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

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/server_options.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(60));

/**
 * Internal sharding command run on config servers to change a database's primary shard.
 */
class ConfigSvrMovePrimaryCommand : public BasicCommand {
public:
    ConfigSvrMovePrimaryCommand() : BasicCommand("_configsvrMovePrimary") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Reassigns the primary shard of a database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        const auto nsElt = cmdObj.firstElement();
        uassert(ErrorCodes::InvalidNamespace,
                "'movePrimary' must be of type String",
                nsElt.type() == BSONType::String);
        return nsElt.str();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname_unused,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrMovePrimary can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        auto movePrimaryRequest =
            MovePrimary::parse(IDLParserErrorContext("ConfigSvrMovePrimary"), cmdObj);
        const auto dbname = parseNs("", cmdObj);

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        if (dbname == NamespaceString::kAdminDb || dbname == NamespaceString::kConfigDb ||
            dbname == NamespaceString::kLocalDb) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Can't move primary for " << dbname << " database");
        }

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "movePrimary must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        const std::string to = movePrimaryRequest.getTo().toString();

        if (to.empty()) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "you have to specify where you want to move it");
        }

        auto const catalogClient = Grid::get(opCtx)->catalogClient();
        auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

        auto dbDistLock = uassertStatusOK(catalogClient->getDistLockManager()->lock(
            opCtx, dbname, "movePrimary", DistLockManager::kDefaultLockTimeout));

        auto dbType =
            uassertStatusOK(catalogClient->getDatabase(
                                opCtx, dbname, repl::ReadConcernArgs::get(opCtx).getLevel()))
                .value;

        const auto fromShard = uassertStatusOK(shardRegistry->getShard(opCtx, dbType.getPrimary()));

        const auto toShard = [&]() {
            auto toShardStatus = shardRegistry->getShard(opCtx, to);
            if (!toShardStatus.isOK()) {
                log() << "Could not move database '" << dbname << "' to shard '" << to
                      << causedBy(toShardStatus.getStatus());
                uassertStatusOKWithContext(
                    toShardStatus.getStatus(),
                    str::stream() << "Could not move database '" << dbname << "' to shard '" << to
                                  << "'");
            }

            return toShardStatus.getValue();
        }();

        if (fromShard->getId() == toShard->getId()) {
            // We did a local read of the database entry above and found that this movePrimary
            // request was already satisfied. However, the data may not be majority committed (a
            // previous movePrimary attempt may have failed with a write concern error).
            // Since the current Client doesn't know the opTime of the last write to the database
            // entry, make it wait for the last opTime in the system when we wait for writeConcern.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            result << "primary" << toShard->toString();
            return true;
        }

        ShardMovePrimary shardMovePrimaryRequest;
        shardMovePrimaryRequest.set_movePrimary(NamespaceString(dbname));
        shardMovePrimaryRequest.setTo(toShard->getId().toString());

        auto cmdResponse = uassertStatusOK(fromShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(
                CommandHelpers::appendPassthroughFields(cmdObj, shardMovePrimaryRequest.toBSON())),
            Shard::RetryPolicy::kIdempotent));

        CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.response, &result);

        return true;
    }

} configsvrMovePrimaryCmd;

}  // namespace
}  // namespace mongo
