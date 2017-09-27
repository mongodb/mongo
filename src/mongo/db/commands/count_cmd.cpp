/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::stringstream;

/**
 * Implements the MongoD side of the count command.
 */
class CmdCount : public BasicCommand {
public:
    CmdCount() : BasicCommand("count") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool slaveOk() const {
        // ok on --slave setups
        return repl::getGlobalReplicationCoordinator()->getSettings().isSlave();
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool maintenanceOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }

    bool supportsNonLocalReadConcern(const std::string& dbName, const BSONObj& cmdObj) const final {
        return true;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    virtual void help(stringstream& help) const {
        help << "count objects in collection";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        if (!authSession->isAuthorizedToParseNamespaceElement(cmdObj.firstElement())) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        const NamespaceString nss(parseNsOrUUID(opCtx, dbname, cmdObj));
        if (!authSession->isAuthorizedForActionsOnNamespace(nss, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual Status explain(OperationContext* opCtx,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainOptions::Verbosity verbosity,
                           BSONObjBuilder* out) const {
        const bool isExplain = true;
        Lock::DBLock dbLock(opCtx, dbname, MODE_IS);
        auto nss = parseNsOrUUID(opCtx, dbname, cmdObj);
        auto request = CountRequest::parseFromBSON(nss, cmdObj, isExplain);
        if (!request.isOK()) {
            return request.getStatus();
        }

        // Acquire the db read lock.
        AutoGetCollectionOrViewForReadCommand ctx(
            opCtx, request.getValue().getNs(), std::move(dbLock));
        Collection* collection = ctx.getCollection();

        if (ctx.getView()) {
            ctx.releaseLocksForView();

            auto viewAggregation = request.getValue().asAggregationCommand();
            if (!viewAggregation.isOK()) {
                return viewAggregation.getStatus();
            }

            auto viewAggRequest = AggregationRequest::parseFromBSON(
                request.getValue().getNs(), viewAggregation.getValue(), verbosity);
            if (!viewAggRequest.isOK()) {
                return viewAggRequest.getStatus();
            }

            return runAggregate(opCtx,
                                viewAggRequest.getValue().getNamespaceString(),
                                viewAggRequest.getValue(),
                                viewAggregation.getValue(),
                                *out);
        }

        // Prevent chunks from being cleaned up during yields - this allows us to only check the
        // version on initial entry into count.
        auto rangePreserver =
            CollectionShardingState::get(opCtx, request.getValue().getNs())->getMetadata();

        auto statusWithPlanExecutor = getExecutorCount(opCtx,
                                                       collection,
                                                       request.getValue(),
                                                       true,  // explain
                                                       PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return statusWithPlanExecutor.getStatus();
        }

        auto exec = std::move(statusWithPlanExecutor.getValue());

        Explain::explainStages(exec.get(), collection, verbosity, out);
        return Status::OK();
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const bool isExplain = false;
        Lock::DBLock dbLock(opCtx, dbname, MODE_IS);
        auto nss = parseNsOrUUID(opCtx, dbname, cmdObj);
        auto request = CountRequest::parseFromBSON(nss, cmdObj, isExplain);
        if (!request.isOK()) {
            return appendCommandStatus(result, request.getStatus());
        }

        AutoGetCollectionOrViewForReadCommand ctx(
            opCtx, request.getValue().getNs(), std::move(dbLock));
        Collection* collection = ctx.getCollection();

        if (ctx.getView()) {
            ctx.releaseLocksForView();

            auto viewAggregation = request.getValue().asAggregationCommand();
            if (!viewAggregation.isOK()) {
                return appendCommandStatus(result, viewAggregation.getStatus());
            }

            BSONObj aggResult = Command::runCommandDirectly(
                opCtx, OpMsgRequest::fromDBAndBody(dbname, std::move(viewAggregation.getValue())));

            if (ResolvedView::isResolvedViewErrorResponse(aggResult)) {
                result.appendElements(aggResult);
                return false;
            }

            ViewResponseFormatter formatter(aggResult);
            Status formatStatus = formatter.appendAsCountResponse(&result);
            if (!formatStatus.isOK()) {
                return appendCommandStatus(result, formatStatus);
            }
            return true;
        }

        // Prevent chunks from being cleaned up during yields - this allows us to only check the
        // version on initial entry into count.
        auto rangePreserver =
            CollectionShardingState::get(opCtx, request.getValue().getNs())->getMetadata();

        auto statusWithPlanExecutor = getExecutorCount(opCtx,
                                                       collection,
                                                       request.getValue(),
                                                       false,  // !explain
                                                       PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }

        auto exec = std::move(statusWithPlanExecutor.getValue());

        // Store the plan summary string in CurOp.
        auto curOp = CurOp::get(opCtx);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
        }

        Status execPlanStatus = exec->executePlan();
        if (!execPlanStatus.isOK()) {
            return appendCommandStatus(result, execPlanStatus);
        }

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
        invariant(STAGE_COUNT == exec->getRootStage()->stageType());
        CountStage* countStage = static_cast<CountStage*>(exec->getRootStage());
        const CountStats* countStats =
            static_cast<const CountStats*>(countStage->getSpecificStats());

        result.appendNumber("n", countStats->nCounted);
        return true;
    }

} cmdCount;

}  // namespace
}  // namespace mongo
