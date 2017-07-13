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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const char kTermField[] = "term";

}  // namespace

/**
 * A command for running .find() queries.
 */
class FindCmd : public BasicCommand {
    MONGO_DISALLOW_COPYING(FindCmd);

public:
    FindCmd() : BasicCommand("find") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return false;
    }

    bool slaveOverrideOk() const override {
        return true;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsReadConcern(const std::string& dbName, const BSONObj& cmdObj) const final {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "query for documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opQuery;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    /**
     * A find command does not increment the command counter, but rather increments the
     * query counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        auto hasTerm = cmdObj.hasField(kTermField);
        return AuthorizationSession::get(client)->checkAuthForFind(nss, hasTerm);
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        if (!nss.isValid()) {
            return {ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid collection name: " << nss.ns()};
        }

        // Parse the command BSON to a QueryRequest.
        const bool isExplain = true;
        auto qrStatus = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
        if (!qrStatus.isOK()) {
            return qrStatus.getStatus();
        }

        // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.

        ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
        auto statusWithCQ =
            CanonicalQuery::canonicalize(opCtx, std::move(qrStatus.getValue()), extensionsCallback);
        if (!statusWithCQ.isOK()) {
            return statusWithCQ.getStatus();
        }
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Acquire locks. If the namespace is a view, we release our locks and convert the query
        // request into an aggregation command.
        AutoGetCollectionOrViewForReadCommand ctx(opCtx, nss);
        if (ctx.getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.releaseLocksForView();

            // Convert the find command into an aggregation using $match (and other stages, as
            // necessary), if possible.
            const auto& qr = cq->getQueryRequest();
            auto viewAggregationCommand = qr.asAggregationCommand();
            if (!viewAggregationCommand.isOK())
                return viewAggregationCommand.getStatus();

            // Create the agg request equivalent of the find operation, with the explain verbosity
            // included.
            auto aggRequest = AggregationRequest::parseFromBSON(
                nss, viewAggregationCommand.getValue(), verbosity);
            if (!aggRequest.isOK()) {
                return aggRequest.getStatus();
            }

            try {
                return runAggregate(
                    opCtx, nss, aggRequest.getValue(), viewAggregationCommand.getValue(), *out);
            } catch (DBException& error) {
                if (error.getCode() == ErrorCodes::InvalidPipelineOperator) {
                    return {ErrorCodes::InvalidPipelineOperator,
                            str::stream() << "Unsupported in view pipeline: " << error.what()};
                }
                return error.toStatus();
            }
        }

        // The collection may be NULL. If so, getExecutor() should handle it by returning an
        // execution tree with an EOFStage.
        Collection* collection = ctx.getCollection();

        // We have a parsed query. Time to get the execution plan for it.
        auto statusWithPlanExecutor =
            getExecutorFind(opCtx, collection, nss, std::move(cq), PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return statusWithPlanExecutor.getStatus();
        }
        auto exec = std::move(statusWithPlanExecutor.getValue());

        // Got the execution tree. Explain it.
        Explain::explainStages(exec.get(), collection, verbosity, out);
        return Status::OK();
    }

    /**
     * Runs a query using the following steps:
     *   --Parsing.
     *   --Acquire locks.
     *   --Plan query, obtaining an executor that can run it.
     *   --Generate the first batch.
     *   --Save state for getMore, transferring ownership of the executor to a ClientCursor.
     *   --Generate response to send to the client.
     */
    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNsOrUUID(opCtx, dbname, cmdObj));

        // Although it is a command, a find command gets counted as a query.
        globalOpCounters.gotQuery();

        // Parse the command BSON to a QueryRequest.
        const bool isExplain = false;
        auto qrStatus = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
        if (!qrStatus.isOK()) {
            return appendCommandStatus(result, qrStatus.getStatus());
        }

        auto& qr = qrStatus.getValue();

        // Validate term before acquiring locks, if provided.
        if (auto term = qr->getReplicationTerm()) {
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            Status status = replCoord->updateTerm(opCtx, *term);
            // Note: updateTerm returns ok if term stayed the same.
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
        }

        // Fill out curop information.
        //
        // We pass negative values for 'ntoreturn' and 'ntoskip' to indicate that these values
        // should be omitted from the log line. Limit and skip information is already present in the
        // find command parameters, so these fields are redundant.
        const int ntoreturn = -1;
        const int ntoskip = -1;
        beginQueryOp(opCtx, nss, cmdObj, ntoreturn, ntoskip);

        // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.
        ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(qr), extensionsCallback);
        if (!statusWithCQ.isOK()) {
            return appendCommandStatus(result, statusWithCQ.getStatus());
        }
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Acquire locks. If the query is on a view, we release our locks and convert the query
        // request into an aggregation command.
        AutoGetCollectionOrViewForReadCommand ctx(opCtx, nss);
        Collection* collection = ctx.getCollection();
        if (ctx.getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.releaseLocksForView();

            // Convert the find command into an aggregation using $match (and other stages, as
            // necessary), if possible.
            const auto& qr = cq->getQueryRequest();
            auto viewAggregationCommand = qr.asAggregationCommand();
            if (!viewAggregationCommand.isOK())
                return appendCommandStatus(result, viewAggregationCommand.getStatus());

            BSONObj aggResult = Command::runCommandDirectly(
                opCtx,
                OpMsgRequest::fromDBAndBody(dbname, std::move(viewAggregationCommand.getValue())));
            auto status = getStatusFromCommandResult(aggResult);
            if (status.code() == ErrorCodes::InvalidPipelineOperator) {
                return appendCommandStatus(
                    result,
                    {ErrorCodes::InvalidPipelineOperator,
                     str::stream() << "Unsupported in view pipeline: " << status.reason()});
            }
            result.resetToEmpty();
            result.appendElements(aggResult);
            return status.isOK();
        }

        // Get the execution plan for the query.
        auto statusWithPlanExecutor =
            getExecutorFind(opCtx, collection, nss, std::move(cq), PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }

        auto exec = std::move(statusWithPlanExecutor.getValue());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
        }

        if (!collection) {
            // No collection. Just fill out curop indicating that there were zero results and
            // there is no ClientCursor id, and then return.
            const long long numResults = 0;
            const CursorId cursorId = 0;
            endQueryOp(opCtx, collection, *exec, numResults, cursorId);
            appendCursorResponseObject(cursorId, nss.ns(), BSONArray(), &result);
            return true;
        }

        const QueryRequest& originalQR = exec->getCanonicalQuery()->getQueryRequest();

        // Stream query results, adding them to a BSONArray as we go.
        CursorResponseBuilder firstBatch(/*isInitialResponse*/ true, &result);
        BSONObj obj;
        PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
        long long numResults = 0;
        while (!FindCommon::enoughForFirstBatch(originalQR, numResults) &&
               PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
            // If we can't fit this result inside the current batch, then we stash it for later.
            if (!FindCommon::haveSpaceForNext(obj, numResults, firstBatch.bytesUsed())) {
                exec->enqueue(obj);
                break;
            }

            // Add result to output buffer.
            firstBatch.append(obj);
            numResults++;
        }

        // Throw an assertion if query execution fails for any reason.
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            firstBatch.abandon();
            error() << "Plan executor error during find command: " << PlanExecutor::statestr(state)
                    << ", stats: " << redact(Explain::getWinningPlanStats(exec.get()));

            return appendCommandStatus(result,
                                       Status(ErrorCodes::OperationFailed,
                                              str::stream()
                                                  << "Executor error during find command: "
                                                  << WorkingSetCommon::toStatusString(obj)));
        }

        // Before saving the cursor, ensure that whatever plan we established happened with the
        // expected collection version
        auto css = CollectionShardingState::get(opCtx, nss);
        css->checkShardVersionOrThrow(opCtx);

        // Set up the cursor for getMore.
        CursorId cursorId = 0;
        if (shouldSaveCursor(opCtx, collection, state, exec.get())) {
            // Create a ClientCursor containing this plan executor and register it with the cursor
            // manager.
            ClientCursorPin pinnedCursor = collection->getCursorManager()->registerCursor(
                opCtx,
                {std::move(exec),
                 nss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                 opCtx->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
                 cmdObj});
            cursorId = pinnedCursor.getCursor()->cursorid();

            invariant(!exec);
            PlanExecutor* cursorExec = pinnedCursor.getCursor()->getExecutor();

            // State will be restored on getMore.
            cursorExec->saveState();
            cursorExec->detachFromOperationContext();

            // We assume that cursors created through a DBDirectClient are always used from their
            // original OperationContext, so we do not need to move time to and from the cursor.
            if (!opCtx->getClient()->isInDirectClient()) {
                pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(
                    opCtx->getRemainingMaxTimeMicros());
            }
            pinnedCursor.getCursor()->setPos(numResults);

            // Fill out curop based on the results.
            endQueryOp(opCtx, collection, *cursorExec, numResults, cursorId);
        } else {
            endQueryOp(opCtx, collection, *exec, numResults, cursorId);
        }

        // Generate the response object to send to the client.
        firstBatch.done(cursorId, nss.ns());
        return true;
    }

} findCmd;

}  // namespace mongo
