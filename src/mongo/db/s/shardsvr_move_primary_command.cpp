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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/s/move_primary_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class MovePrimaryCommand : public BasicCommand {
public:
    MovePrimaryCommand() : BasicCommand("_shardsvrMovePrimary") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

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

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        const auto nsElt = cmdObj.firstElement();
        uassert(ErrorCodes::InvalidNamespace,
                "'movePrimary' must be of type String",
                nsElt.type() == BSONType::String);
        return NamespaceStringUtil::parseNamespaceFromRequest(dbName.tenantId(), nsElt.str());
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

        const auto movePrimaryRequest =
            ShardMovePrimary::parse(IDLParserContext("_shardsvrMovePrimary"), cmdObj);
        const auto dbName = parseNs(DatabaseName(), cmdObj).dbName();

        const NamespaceString dbNss(dbName);
        const auto toShardArg = movePrimaryRequest.getTo();

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbName.toStringForErrorMsg(),
            NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow));

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Can't move primary for " << dbName.toStringForErrorMsg()
                              << " database",
                !dbNss.isOnInternalDb());

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "you have to specify where you want to move it",
                !toShardArg.empty());

        CommandHelpers::uassertCommandRunWithMajority(getName(), opCtx->getWriteConcern());

        ON_BLOCK_EXIT(
            [opCtx, dbNss] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbNss.db()); });

        const auto coordinatorFuture = [&] {
            FixedFCVRegion fcvRegion(opCtx);

            auto shardRegistry = Grid::get(opCtx)->shardRegistry();
            // Ensure that the shard information is up-to-date as possible to catch the case where
            // a shard with the same name, but with a different host, has been removed/re-added.
            shardRegistry->reload(opCtx);
            const auto toShard = uassertStatusOKWithContext(
                shardRegistry->getShard(opCtx, toShardArg.toString()),
                "Requested primary shard {} does not exist"_format(toShardArg));

            const auto coordinatorDoc = [&] {
                MovePrimaryCoordinatorDocument doc;
                doc.setShardingDDLCoordinatorMetadata(
                    {{dbNss, DDLCoordinatorTypeEnum::kMovePrimary}});
                doc.setToShardId(toShard->getId());
                return doc.toBSON();
            }();

            const auto coordinator = [&] {
                auto service = ShardingDDLCoordinatorService::getService(opCtx);
                return checked_pointer_cast<MovePrimaryCoordinator>(
                    service->getOrCreateInstance(opCtx, std::move(coordinatorDoc)));
            }();

            return coordinator->getCompletionFuture();
        }();

        coordinatorFuture.get(opCtx);
        return true;
    }
} movePrimaryCmd;

}  // namespace
}  // namespace mongo
