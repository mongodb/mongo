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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/active_move_primaries_registry.h"
#include "mongo/db/s/move_primary_source_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * If the specified status is not OK logs a warning and throws a DBException corresponding to the
 * specified status.
 */
void uassertStatusOKWithWarning(const Status& status) {
    if (!status.isOK()) {
        warning() << "movePrimary failed" << causedBy(redact(status));
        uassertStatusOK(status);
    }
}

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

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
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
        auto const shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        const auto movePrimaryRequest =
            ShardMovePrimary::parse(IDLParserErrorContext("_movePrimary"), cmdObj);
        const auto dbname = parseNs("", cmdObj);

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Can't move primary for a system database " << dbname,
                dbname != NamespaceString::kAdminDb && dbname != NamespaceString::kConfigDb &&
                    dbname != NamespaceString::kLocalDb);

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "_movePrimary must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        uassert(ErrorCodes::InvalidOptions,
                "you have to specify where you want to move it",
                !movePrimaryRequest.getTo().empty());

        // Make sure we're as up-to-date as possible with shard information. This catches the case
        // where we might have changed a shard's host by removing/adding a shard with the same name.
        Grid::get(opCtx)->shardRegistry()->reload(opCtx);

        auto scopedMovePrimary = uassertStatusOK(
            ActiveMovePrimariesRegistry::get(opCtx).registerMovePrimary(movePrimaryRequest));

        // Check if there is an existing movePrimary running and if so, join it
        if (scopedMovePrimary.mustExecute()) {
            auto status = [&] {
                try {
                    _runImpl(opCtx, movePrimaryRequest, dbname);
                    return Status::OK();
                } catch (const DBException& ex) {
                    return ex.toStatus();
                }
            }();
            scopedMovePrimary.signalComplete(status);
            uassertStatusOK(status);
        } else {
            uassertStatusOK(scopedMovePrimary.waitForCompletion(opCtx));
        }

        return true;
    }

private:
    static void _runImpl(OperationContext* opCtx,
                         const ShardMovePrimary movePrimaryRequest,
                         const StringData dbname) {
        ShardId fromShardId = ShardingState::get(opCtx)->shardId();
        ShardId toShardId = movePrimaryRequest.getTo().toString();

        MovePrimarySourceManager movePrimarySourceManager(
            opCtx, movePrimaryRequest, dbname, fromShardId, toShardId);

        uassertStatusOKWithWarning(movePrimarySourceManager.clone(opCtx));
        uassertStatusOKWithWarning(movePrimarySourceManager.enterCriticalSection(opCtx));
        uassertStatusOKWithWarning(movePrimarySourceManager.commitOnConfig(opCtx));
        uassertStatusOKWithWarning(movePrimarySourceManager.cleanStaleData(opCtx));
    }

} movePrimaryCmd;

}  // namespace
}  // namespace mongo
