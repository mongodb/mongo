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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/count_command_as_aggregation_command.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

using std::string;
using std::stringstream;
using std::unique_ptr;

// Failpoint which causes to hang "count" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeCollectionCount);

/**
 * Implements the MongoD side of the count command.
 */
class CmdCount : public BasicCommand {
public:
    CmdCount() : BasicCommand("count") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    std::string help() const override {
        return "count objects in collection";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
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

    bool allowedWithSecurityToken() const final {
        return true;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        static const Status kSnapshotNotSupported{ErrorCodes::InvalidOptions,
                                                  "read concern snapshot not supported"};
        return {{level == repl::ReadConcernLevel::kSnapshotReadConcern, kSnapshotNotSupported},
                Status::OK()};
    }

    bool shouldAffectReadConcernCounter() const override {
        return true;
    }

    bool supportsReadMirroring(const BSONObj&) const override {
        return true;
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
        return auth::checkAuthForFind(
            authSession,
            CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(
                opCtx, CommandHelpers::parseNsOrUUID({boost::none, dbname}, cmdObj)),
            hasTerm);
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& opMsgRequest,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        DatabaseName dbName(opMsgRequest.getValidatedTenantId(), opMsgRequest.getDatabase());
        const BSONObj& cmdObj = opMsgRequest.body;
        // Acquire locks. The RAII object is optional, because in the case
        // of a view, the locks need to be released.
        boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> ctx;
        ctx.emplace(opCtx,
                    CommandHelpers::parseNsCollectionRequired(dbName, cmdObj),
                    AutoGetCollectionViewMode::kViewsPermitted);
        const auto nss = ctx->getNss();

        CountCommandRequest request(NamespaceStringOrUUID(NamespaceString{}));
        try {
            request = CountCommandRequest::parse(IDLParserContext("count"), opMsgRequest);
        } catch (...) {
            return exceptionToStatus();
        }

        if (shouldDoFLERewrite(request)) {
            processFLECountD(opCtx, nss, &request);
        }

        if (ctx->getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            auto viewAggregation = countCommandAsAggregationCommand(request, nss);
            if (!viewAggregation.isOK()) {
                return viewAggregation.getStatus();
            }

            auto viewAggCmd =
                OpMsgRequest::fromDBAndBody(nss.db(), viewAggregation.getValue()).body;
            auto viewAggRequest = aggregation_request_helper::parseFromBSON(
                opCtx,
                nss,
                viewAggCmd,
                verbosity,
                APIParameters::get(opCtx).getAPIStrict().value_or(false));

            // An empty PrivilegeVector is acceptable because these privileges are only checked on
            // getMore and explain will not open a cursor.
            return runAggregate(opCtx,
                                viewAggRequest.getNamespace(),
                                viewAggRequest,
                                viewAggregation.getValue(),
                                PrivilegeVector(),
                                result);
        }

        const auto& collection = ctx->getCollection();

        // Prevent chunks from being cleaned up during yields - this allows us to only check the
        // version on initial entry into count.
        boost::optional<ScopedCollectionFilter> rangePreserver;
        if (collection.isSharded()) {
            rangePreserver.emplace(
                CollectionShardingState::getSharedForLockFreeReads(opCtx, nss)
                    ->getOwnershipFilter(
                        opCtx,
                        CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup));
        }

        auto expCtx = makeExpressionContextForGetExecutor(
            opCtx, request.getCollation().value_or(BSONObj()), nss);

        auto statusWithPlanExecutor =
            getExecutorCount(expCtx, &collection, request, true /*explain*/, nss);
        if (!statusWithPlanExecutor.isOK()) {
            return statusWithPlanExecutor.getStatus();
        }

        auto exec = std::move(statusWithPlanExecutor.getValue());

        auto bodyBuilder = result->getBodyBuilder();
        Explain::explainStages(exec.get(), collection, verbosity, BSONObj(), cmdObj, &bodyBuilder);
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        // Acquire locks and resolve possible UUID. The RAII object is optional, because in the case
        // of a view, the locks need to be released.
        boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> ctx;
        ctx.emplace(opCtx,
                    CommandHelpers::parseNsOrUUID(dbName, cmdObj),
                    AutoGetCollectionViewMode::kViewsPermitted);
        const auto& nss = ctx->getNss();

        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeCollectionCount, opCtx, "hangBeforeCollectionCount", []() {}, nss);

        auto request = CountCommandRequest::parse(IDLParserContext("count"), cmdObj);
        if (shouldDoFLERewrite(request)) {
            processFLECountD(opCtx, nss, &request);
        }
        if (request.getMirrored().value_or(false)) {
            const auto& invocation = CommandInvocation::get(opCtx);
            invocation->markMirrored();
        }

        if (ctx->getView()) {
            auto viewAggregation = countCommandAsAggregationCommand(request, nss);

            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            uassertStatusOK(viewAggregation.getStatus());

            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                opCtx,
                OpMsgRequest::fromDBAndBody(dbName.db(), std::move(viewAggregation.getValue())));

            uassertStatusOK(ViewResponseFormatter(aggResult).appendAsCountResponse(&result));
            return true;
        }

        // Check whether we are allowed to read from this node after acquiring our locks.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkCanServeReadsFor(
            opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        const auto& collection = ctx->getCollection();

        // Prevent chunks from being cleaned up during yields - this allows us to only check the
        // version on initial entry into count.
        boost::optional<ScopedCollectionFilter> rangePreserver;
        if (collection.isSharded()) {
            rangePreserver.emplace(
                CollectionShardingState::getSharedForLockFreeReads(opCtx, nss)
                    ->getOwnershipFilter(
                        opCtx,
                        CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup));
        }

        auto statusWithPlanExecutor =
            getExecutorCount(makeExpressionContextForGetExecutor(
                                 opCtx, request.getCollation().value_or(BSONObj()), nss),
                             &collection,
                             request,
                             false /*explain*/,
                             nss);
        uassertStatusOK(statusWithPlanExecutor.getStatus());

        auto exec = std::move(statusWithPlanExecutor.getValue());

        // Store the plan summary string in CurOp.
        auto curOp = CurOp::get(opCtx);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
        }

        auto countResult = exec->executeCount();

        PlanSummaryStats summaryStats;
        exec->getPlanExplainer().getSummaryStats(&summaryStats);
        if (collection) {
            CollectionQueryInfo::get(collection).notifyOfQuery(opCtx, collection, summaryStats);
        }
        curOp->debug().setPlanSummaryMetrics(summaryStats);

        if (curOp->shouldDBProfile(opCtx)) {
            auto&& explainer = exec->getPlanExplainer();
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            curOp->debug().execStats = std::move(stats);
        }

        result.appendNumber("n", countResult);
        return true;
    }

    void appendMirrorableRequest(BSONObjBuilder* bob, const BSONObj& cmdObj) const override {
        static const auto kMirrorableKeys = [] {
            BSONObjBuilder keyBob;

            keyBob.append("count", 1);
            keyBob.append("query", 1);
            keyBob.append("skip", 1);
            keyBob.append("limit", 1);
            keyBob.append("hint", 1);
            keyBob.append("collation", 1);
            keyBob.append("shardVersion", 1);

            return keyBob.obj();
        }();

        // Filter the keys that can be mirrored
        cmdObj.filterFieldsUndotted(bob, kMirrorableKeys, true);
    }

} cmdCount;

}  // namespace
}  // namespace mongo
