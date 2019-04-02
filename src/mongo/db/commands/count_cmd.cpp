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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::stringstream;

// Failpoint which causes to hang "count" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeCollectionCount);

/**
 * Implements the MongoD side of the count command.
 */
class CmdCount : public BasicCommand {
public:
    CmdCount() : BasicCommand("count") {}

    std::string help() const override {
        return "count objects in collection";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool canIgnorePrepareConflicts() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* serviceContext) const override {
        return Command::AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const override {
        return level != repl::ReadConcernLevel::kSnapshotReadConcern;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        if (!authSession->isAuthorizedToParseNamespaceElement(cmdObj.firstElement())) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        const auto hasTerm = false;
        return authSession->checkAuthForFind(
            AutoGetCollection::resolveNamespaceStringOrUUID(
                opCtx, CommandHelpers::parseNsOrUUID(dbname, cmdObj)),
            hasTerm);
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& opMsgRequest,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        std::string dbname = opMsgRequest.getDatabase().toString();
        const BSONObj& cmdObj = opMsgRequest.body;
        // Acquire locks. The RAII object is optional, because in the case
        // of a view, the locks need to be released.
        boost::optional<AutoGetCollectionForReadCommand> ctx;
        ctx.emplace(opCtx,
                    CommandHelpers::parseNsCollectionRequired(dbname, cmdObj),
                    AutoGetCollection::ViewMode::kViewsPermitted);
        const auto nss = ctx->getNss();

        const bool isExplain = true;
        auto request = CountRequest::parseFromBSON(nss, cmdObj, isExplain);
        if (!request.isOK()) {
            return request.getStatus();
        }

        if (ctx->getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            auto viewAggregation = request.getValue().asAggregationCommand();
            if (!viewAggregation.isOK()) {
                return viewAggregation.getStatus();
            }

            auto viewAggRequest = AggregationRequest::parseFromBSON(
                request.getValue().getNs(), viewAggregation.getValue(), verbosity);
            if (!viewAggRequest.isOK()) {
                return viewAggRequest.getStatus();
            }

            // An empty PrivilegeVector is acceptable because these privileges are only checked on
            // getMore and explain will not open a cursor.
            return runAggregate(opCtx,
                                viewAggRequest.getValue().getNamespaceString(),
                                viewAggRequest.getValue(),
                                viewAggregation.getValue(),
                                PrivilegeVector(),
                                result);
        }

        Collection* const collection = ctx->getCollection();

        // Prevent chunks from being cleaned up during yields - this allows us to only check the
        // version on initial entry into count.
        auto rangePreserver = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();

        auto statusWithPlanExecutor =
            getExecutorCount(opCtx, collection, request.getValue(), true /*explain*/);
        if (!statusWithPlanExecutor.isOK()) {
            return statusWithPlanExecutor.getStatus();
        }

        auto exec = std::move(statusWithPlanExecutor.getValue());

        auto bodyBuilder = result->getBodyBuilder();
        Explain::explainStages(exec.get(), collection, verbosity, BSONObj(), &bodyBuilder);
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        // Acquire locks and resolve possible UUID. The RAII object is optional, because in the case
        // of a view, the locks need to be released.
        boost::optional<AutoGetCollectionForReadCommand> ctx;
        ctx.emplace(opCtx,
                    CommandHelpers::parseNsOrUUID(dbname, cmdObj),
                    AutoGetCollection::ViewMode::kViewsPermitted);
        const auto& nss = ctx->getNss();

        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeCollectionCount, opCtx, "hangBeforeCollectionCount", []() {}, false, nss);

        const bool isExplain = false;
        auto request = CountRequest::parseFromBSON(nss, cmdObj, isExplain);
        uassertStatusOK(request.getStatus());

        // Check whether we are allowed to read from this node after acquiring our locks.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkCanServeReadsFor(
            opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        if (ctx->getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            auto viewAggregation = request.getValue().asAggregationCommand();
            uassertStatusOK(viewAggregation.getStatus());

            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                opCtx, OpMsgRequest::fromDBAndBody(dbname, std::move(viewAggregation.getValue())));

            uassertStatusOK(ViewResponseFormatter(aggResult).appendAsCountResponse(&result));
            return true;
        }

        Collection* const collection = ctx->getCollection();

        // Prevent chunks from being cleaned up during yields - this allows us to only check the
        // version on initial entry into count.
        auto rangePreserver = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();

        auto statusWithPlanExecutor =
            getExecutorCount(opCtx, collection, request.getValue(), false /*explain*/);
        uassertStatusOK(statusWithPlanExecutor.getStatus());

        auto exec = std::move(statusWithPlanExecutor.getValue());

        // Store the plan summary string in CurOp.
        auto curOp = CurOp::get(opCtx);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
        }

        Status execPlanStatus = exec->executePlan();
        uassertStatusOK(execPlanStatus);

        PlanSummaryStats summaryStats;
        Explain::getSummaryStats(*exec, &summaryStats);
        if (collection) {
            collection->infoCache()->notifyOfQuery(opCtx, summaryStats.indexesUsed);
        }
        curOp->debug().setPlanSummaryMetrics(summaryStats);

        if (curOp->shouldDBProfile()) {
            BSONObjBuilder execStatsBob;
            Explain::getWinningPlanStats(exec.get(), &execStatsBob);
            curOp->debug().execStats = execStatsBob.obj();
        }

        // Plan is done executing. We just need to pull the count out of the root stage.
        invariant(STAGE_COUNT == exec->getRootStage()->stageType() ||
                  STAGE_RECORD_STORE_FAST_COUNT == exec->getRootStage()->stageType());
        auto* countStats = static_cast<const CountStats*>(exec->getRootStage()->getSpecificStats());

        result.appendNumber("n", countStats->nCounted);
        return true;
    }

} cmdCount;

}  // namespace
}  // namespace mongo
