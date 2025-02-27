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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/coll_mod_reply_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/s/collmod_coordinator.h"
#include "mongo/db/s/collmod_coordinator_pre60_compatible.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(collModPrimaryDispatching);
MONGO_FAIL_POINT_DEFINE(collModCoordinatorPre60Compatible);

class ShardsvrCollModCommand final : public BasicCommandWithRequestParser<ShardsvrCollModCommand> {
public:
    using Request = ShardsvrCollMod;
    using Response = CollModReply;

    ShardsvrCollModCommand() : BasicCommandWithRequestParser() {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Modifies collection.";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return auth::checkAuthForCollMod(
            client->getOperationContext(), AuthorizationSession::get(client), nss, cmdObj, false);
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const std::string& db,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) override {
        auto const shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                      opCtx->getWriteConcern());

        opCtx->setAlwaysInterruptAtStepDownOrUp();

        // Since this operation is not directly writing locally we need to force its db
        // profile level increase in order to be logged in "<db>.system.profile"
        const auto& cmd = requestParser.request();
        CurOp::get(opCtx)->raiseDbProfileLevel(
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(cmd.getDbName()));

        boost::optional<FixedFCVRegion> fcvRegion;
        fcvRegion.emplace(opCtx);

        if (MONGO_unlikely(collModPrimaryDispatching.shouldFail())) {
            return runWithDispatchingCommands(opCtx, result, cmd);
        } else if (MONGO_unlikely(collModCoordinatorPre60Compatible.shouldFail())) {
            return runWithDDLCoordinatorPre60Compatible(opCtx, result, cmd, fcvRegion);
        }

        if (fcvRegion.get()->isLessThan(multiversion::FeatureCompatibilityVersion::kVersion_5_3)) {
            return runWithDispatchingCommands(opCtx, result, cmd);
        } else if (fcvRegion.get()->isLessThan(
                       multiversion::FeatureCompatibilityVersion::kVersion_6_0)) {
            return runWithDDLCoordinatorPre60Compatible(opCtx, result, cmd, fcvRegion);
        } else {
            return runWithDDLCoordinator(opCtx, result, cmd, fcvRegion);
        }
    }

    bool runWithDDLCoordinator(OperationContext* opCtx,
                               BSONObjBuilder& result,
                               const ShardsvrCollMod& cmd,
                               boost::optional<FixedFCVRegion>& fcvRegion) {
        auto coordinatorDoc = CollModCoordinatorDocument();
        coordinatorDoc.setCollModRequest(cmd.getCollModRequest());
        coordinatorDoc.setShardingDDLCoordinatorMetadata(
            {{cmd.getNamespace(), DDLCoordinatorTypeEnum::kCollMod}});
        auto service = ShardingDDLCoordinatorService::getService(opCtx);
        auto collModCoordinator = checked_pointer_cast<CollModCoordinator>(
            service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON()));
        fcvRegion = boost::none;
        result.appendElements(collModCoordinator->getResult(opCtx));
        return true;
    }

    bool runWithDDLCoordinatorPre60Compatible(OperationContext* opCtx,
                                              BSONObjBuilder& result,
                                              const ShardsvrCollMod& cmd,
                                              boost::optional<FixedFCVRegion>& fcvRegion) {
        auto coordinatorDoc = CollModCoordinatorDocument();
        coordinatorDoc.setCollModRequest(cmd.getCollModRequest());
        coordinatorDoc.setShardingDDLCoordinatorMetadata(
            {{cmd.getNamespace(), DDLCoordinatorTypeEnum::kCollModPre60Compatible}});
        auto service = ShardingDDLCoordinatorService::getService(opCtx);
        auto collModCoordinator = checked_pointer_cast<CollModCoordinatorPre60Compatible>(
            service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON()));
        fcvRegion = boost::none;
        result.appendElements(collModCoordinator->getResult(opCtx));
        return true;
    }

    bool runWithDispatchingCommands(OperationContext* opCtx,
                                    BSONObjBuilder& result,
                                    const ShardsvrCollMod& cmd) {
        const auto& nss = cmd.getNamespace();
        auto collModCmd = CollMod(nss);
        collModCmd.setCollModRequest(cmd.getCollModRequest());
        auto collModCmdObj = collModCmd.toBSON({});

        const auto targeter = ChunkManagerTargeter(opCtx, nss);
        const auto& routingInfo = targeter.getRoutingInfo();
        if (targeter.timeseriesNamespaceNeedsRewrite(nss)) {
            collModCmdObj =
                timeseries::makeTimeseriesCommand(collModCmdObj,
                                                  nss,
                                                  CollMod::kCommandName,
                                                  CollMod::kIsTimeseriesNamespaceFieldName);
        }

        std::set<ShardId> participants;
        if (routingInfo.isSharded()) {
            std::unique_ptr<CollatorInterface> collator;
            const auto expCtx =
                make_intrusive<ExpressionContext>(opCtx, std::move(collator), targeter.getNS());
            routingInfo.getShardIdsForQuery(
                expCtx, {} /* query */, {} /* collation */, &participants);
        } else {
            participants.insert(routingInfo.dbPrimary());
        }

        auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
        const auto& responses = sharding_util::sendCommandToShards(
            opCtx,
            targeter.getNS().db(),
            CommandHelpers::appendMajorityWriteConcern(collModCmdObj, opCtx->getWriteConcern()),
            {std::make_move_iterator(participants.begin()),
             std::make_move_iterator(participants.end())},
            executor);

        std::string errmsg;
        auto ok = appendRawResponses(opCtx, &errmsg, &result, std::move(responses)).responseOK;
        if (!errmsg.empty()) {
            CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);
        }
        return ok;
    }

    void validateResult(const BSONObj& resultObj) final {
        StringDataSet ignorableFields({"raw", "ok", "errmsg"});
        auto reply = Response::parse(IDLParserErrorContext("CollModReply"),
                                     resultObj.removeFields(ignorableFields));
        coll_mod_reply_validation::validateReply(reply);
    }
} shardsvrCollModCommand;

}  // namespace
}  // namespace mongo
