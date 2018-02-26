/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class MovePrimaryCommand : public BasicCommand {
public:
    MovePrimaryCommand() : BasicCommand("_movePrimary") {}

    std::string help() const override {
        return "should not be calling this directly";
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
        if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
            return CommandHelpers::appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation,
                       "_movePrimary can only be run on shard servers"));
        }

        auto shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        const auto movePrimaryRequest =
            ShardMovePrimary::parse(IDLParserErrorContext("_movePrimary"), cmdObj);
        const auto dbname = parseNs("", cmdObj);

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        if (dbname == NamespaceString::kAdminDb || dbname == NamespaceString::kConfigDb ||
            dbname == NamespaceString::kLocalDb) {
            return CommandHelpers::appendCommandStatus(
                result,
                {ErrorCodes::InvalidOptions,
                 str::stream() << "Can't move primary for " << dbname << " database"});
        }

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "_movePrimary must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        const std::string to = movePrimaryRequest.getTo().toString();

        if (to.empty()) {
            return CommandHelpers::appendCommandStatus(
                result,
                {ErrorCodes::InvalidOptions,
                 str::stream() << "you have to specify where you want to move it"});
        }

        // Make sure we're as up-to-date as possible with shard information. This catches the case
        // where we might have changed a shard's host by removing/adding a shard with the same name.
        Grid::get(opCtx)->shardRegistry()->reload(opCtx);

        auto scopedMovePrimary =
            uassertStatusOK(shardingState->registerMovePrimary(movePrimaryRequest));

        Status status = {ErrorCodes::InternalError, "Uninitialized value"};

        // Check if there is an existing movePrimary running and if so, join it
        if (scopedMovePrimary.mustExecute()) {
            status = Status::OK();
            scopedMovePrimary.signalComplete(status);
        } else {
            status = scopedMovePrimary.waitForCompletion(opCtx);
        }

        uassertStatusOK(status);

        return true;
    }

} movePrimaryCmd;

}  // namespace
}  // namespace mongo
