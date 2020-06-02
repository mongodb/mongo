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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto kTermField = "term"_sd;

// Parses the command object to a QueryRequest. If the client request did not specify any runtime
// constants, make them available to the query here.
std::unique_ptr<QueryRequest> parseCmdObjectToQueryRequest(OperationContext* opCtx,
                                                           NamespaceString nss,
                                                           BSONObj cmdObj,
                                                           bool isExplain) {
    auto qr = uassertStatusOK(
        QueryRequest::makeFromFindCommand(std::move(nss), std::move(cmdObj), isExplain));
    if (!qr->getRuntimeConstants()) {
        qr->setRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
    }
    return qr;
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContext(OperationContext* opCtx,
                                                              const QueryRequest& queryRequest) {
    std::unique_ptr<CollatorInterface> collator;
    if (!queryRequest.getCollation().isEmpty()) {
        collator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                       ->makeFromBSON(queryRequest.getCollation()));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx, std::move(collator), queryRequest.getRuntimeConstants()));
    expCtx->startExpressionCounters();

    return expCtx;
}

/**
 * A command for running .find() queries.
 */
class FindCmd final : public Command {
public:
    FindCmd() : Command("find") {}

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        // TODO: Parse into a QueryRequest here.
        return std::make_unique<Invocation>(this, opMsgRequest, opMsgRequest.getDatabase());
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "query for documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opQuery;
    }

    ReadWriteType getReadWriteType() const override {
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

    class Invocation final : public CommandInvocation {
    public:
        Invocation(const FindCmd* definition, const OpMsgRequest& request, StringData dbName)
            : CommandInvocation(definition), _request(request), _dbName(dbName) {}

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        bool supportsReadConcern(repl::ReadConcernLevel level) const final {
            return true;
        }

        bool canIgnorePrepareConflicts() const override {
            return true;
        }

        bool allowsSpeculativeMajorityReads() const override {
            // Find queries are only allowed to use speculative behavior if the 'allowsSpeculative'
            // flag is passed. The find command will check for this flag internally and fail if
            // necessary.
            return true;
        }

        NamespaceString ns() const override {
            // TODO get the ns from the parsed QueryRequest.
            return NamespaceString(CommandHelpers::parseNsFromCommand(_dbName, _request.body));
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    authSession->isAuthorizedToParseNamespaceElement(_request.body.firstElement()));

            const auto hasTerm = _request.body.hasField(kTermField);
            uassertStatusOK(authSession->checkAuthForFind(
                AutoGetCollection::resolveNamespaceStringOrUUID(
                    opCtx, CommandHelpers::parseNsOrUUID(_dbName, _request.body)),
                hasTerm));
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            // Acquire locks. The RAII object is optional, because in the case of a view, the locks
            // need to be released.
            boost::optional<AutoGetCollectionForReadCommand> ctx;
            ctx.emplace(opCtx,
                        CommandHelpers::parseNsCollectionRequired(_dbName, _request.body),
                        AutoGetCollection::ViewMode::kViewsPermitted);
            const auto nss = ctx->getNss();

            // Parse the command BSON to a QueryRequest.
            const bool isExplain = true;
            auto qr = parseCmdObjectToQueryRequest(opCtx, nss, _request.body, isExplain);

            // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.
            const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
            auto expCtx = makeExpressionContext(opCtx, *qr);
            auto cq = uassertStatusOK(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(qr),
                                             expCtx,
                                             extensionsCallback,
                                             MatchExpressionParser::kAllowAllSpecialFeatures));

            if (ctx->getView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                ctx.reset();

                // Convert the find command into an aggregation using $match (and other stages, as
                // necessary), if possible.
                const auto& qr = cq->getQueryRequest();
                auto viewAggregationCommand = uassertStatusOK(qr.asAggregationCommand());

                // Create the agg request equivalent of the find operation, with the explain
                // verbosity included.
                auto aggRequest = uassertStatusOK(
                    AggregationRequest::parseFromBSON(nss, viewAggregationCommand, verbosity));

                try {
                    // An empty PrivilegeVector is acceptable because these privileges are only
                    // checked on getMore and explain will not open a cursor.
                    uassertStatusOK(runAggregate(
                        opCtx, nss, aggRequest, viewAggregationCommand, PrivilegeVector(), result));
                } catch (DBException& error) {
                    if (error.code() == ErrorCodes::InvalidPipelineOperator) {
                        uasserted(ErrorCodes::InvalidPipelineOperator,
                                  str::stream()
                                      << "Unsupported in view pipeline: " << error.what());
                    }
                    throw;
                }
                return;
            }

            // The collection may be NULL. If so, getExecutor() should handle it by returning an
            // execution tree with an EOFStage.
            Collection* const collection = ctx->getCollection();

            // Get the execution plan for the query.
            bool permitYield = true;
            auto exec =
                uassertStatusOK(getExecutorFind(opCtx, collection, std::move(cq), permitYield));

            auto bodyBuilder = result->getBodyBuilder();
            // Got the execution tree. Explain it.
            Explain::explainStages(exec.get(), collection, verbosity, BSONObj(), &bodyBuilder);
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
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            // Although it is a command, a find command gets counted as a query.
            globalOpCounters.gotQuery();
            ServerReadConcernMetrics::get(opCtx)->recordReadConcern(
                repl::ReadConcernArgs::get(opCtx));

            // Parse the command BSON to a QueryRequest. Pass in the parsedNss in case _request.body
            // does not have a UUID.
            auto parsedNss =
                NamespaceString{CommandHelpers::parseNsFromCommand(_dbName, _request.body)};
            const bool isExplain = false;
            auto qr =
                parseCmdObjectToQueryRequest(opCtx, std::move(parsedNss), _request.body, isExplain);

            // Only allow speculative majority for internal commands that specify the correct flag.
            uassert(ErrorCodes::ReadConcernMajorityNotEnabled,
                    "Majority read concern is not enabled.",
                    !(repl::ReadConcernArgs::get(opCtx).isSpeculativeMajority() &&
                      !qr->allowSpeculativeMajorityRead()));

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "It is illegal to open a tailable cursor in a transaction",
                    !(opCtx->inMultiDocumentTransaction() && qr->isTailable()));

            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "The 'readOnce' option is not supported within a transaction.",
                    !txnParticipant || !opCtx->inMultiDocumentTransaction() || !qr->isReadOnce());

            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalReadAtClusterTime' option is only supported when testing"
                    " commands are enabled",
                    !qr->getReadAtClusterTime() || getTestCommandsEnabled());

            uassert(
                ErrorCodes::OperationNotSupportedInTransaction,
                "The '$_internalReadAtClusterTime' option is not supported within a transaction.",
                !txnParticipant || !opCtx->inMultiDocumentTransaction() ||
                    !qr->getReadAtClusterTime());

            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalReadAtClusterTime' option is only supported when replication is"
                    " enabled",
                    !qr->getReadAtClusterTime() || replCoord->isReplEnabled());

            auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalReadAtClusterTime' option is only supported by storage engines"
                    " that support document-level concurrency",
                    !qr->getReadAtClusterTime() || storageEngine->supportsDocLocking());

            // Validate term before acquiring locks, if provided.
            auto term = qr->getReplicationTerm();
            if (term) {
                // Note: updateTerm returns ok if term stayed the same.
                uassertStatusOK(replCoord->updateTerm(opCtx, *term));
            }

            // The presence of a term in the request indicates that this is an internal replication
            // oplog read request.
            if (term && parsedNss == NamespaceString::kRsOplogNamespace) {
                // We do not want to take tickets for internal (replication) oplog reads. Stalling
                // on ticket acquisition can cause complicated deadlocks. Primaries may depend on
                // data reaching secondaries in order to proceed; and secondaries may get stalled
                // replicating because of an inability to acquire a read ticket.
                opCtx->lockState()->skipAcquireTicket();
            }

            // We call RecoveryUnit::setTimestampReadSource() before acquiring a lock on the
            // collection via AutoGetCollectionForRead in order to ensure the comparison to the
            // collection's minimum visible snapshot is accurate.
            if (auto targetClusterTime = qr->getReadAtClusterTime()) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "$_internalReadAtClusterTime value must not be a null"
                                         " timestamp.",
                        !targetClusterTime->isNull());

                // We aren't holding the global lock in intent mode, so it is possible after
                // comparing 'targetClusterTime' to 'lastAppliedOpTime' for the last applied opTime
                // to go backwards or for the term to change due to replication rollback. This isn't
                // an actual concern because the testing infrastructure won't use the
                // $_internalReadAtClusterTime option in any test suite where rollback is expected
                // to occur.
                auto lastAppliedOpTime = replCoord->getMyLastAppliedOpTime();

                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "$_internalReadAtClusterTime value must not be greater"
                                         " than the last applied opTime. Requested clusterTime: "
                                      << targetClusterTime->toString()
                                      << "; last applied opTime: " << lastAppliedOpTime.toString(),
                        lastAppliedOpTime.getTimestamp() >= targetClusterTime);

                // We aren't holding the global lock in intent mode, so it is possible for the
                // global storage engine to have been destructed already as a result of the server
                // shutting down. This isn't an actual concern because the testing infrastructure
                // won't use the $_internalReadAtClusterTime option in any test suite where clean
                // shutdown is expected to occur concurrently with tests running.
                auto allDurableTime = storageEngine->getAllDurableTimestamp();
                invariant(!allDurableTime.isNull());

                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "$_internalReadAtClusterTime value must not be greater"
                                         " than the all_durable timestamp. Requested"
                                         " clusterTime: "
                                      << targetClusterTime->toString()
                                      << "; all_durable timestamp: " << allDurableTime.toString(),
                        allDurableTime >= targetClusterTime);

                // The $_internalReadAtClusterTime option causes any storage-layer cursors created
                // during plan execution to read from a consistent snapshot of data at the supplied
                // clusterTime, even across yields.
                opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                              targetClusterTime);

                // The $_internalReadAtClusterTime option also causes any storage-layer cursors
                // created during plan execution to block on prepared transactions. Since the find
                // command ignores prepare conflicts by default, change the behavior.
                opCtx->recoveryUnit()->setPrepareConflictBehavior(
                    PrepareConflictBehavior::kEnforce);
            }

            // Acquire locks. If the query is on a view, we release our locks and convert the query
            // request into an aggregation command.
            boost::optional<AutoGetCollectionForReadCommand> ctx;
            ctx.emplace(opCtx,
                        CommandHelpers::parseNsOrUUID(_dbName, _request.body),
                        AutoGetCollection::ViewMode::kViewsPermitted);
            const auto& nss = ctx->getNss();

            qr->refreshNSS(opCtx);

            // Check whether we are allowed to read from this node after acquiring our locks.
            uassertStatusOK(replCoord->checkCanServeReadsFor(
                opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

            // Fill out curop information.
            //
            // We pass negative values for 'ntoreturn' and 'ntoskip' to indicate that these values
            // should be omitted from the log line. Limit and skip information is already present in
            // the find command parameters, so these fields are redundant.
            const int ntoreturn = -1;
            const int ntoskip = -1;
            beginQueryOp(opCtx, nss, _request.body, ntoreturn, ntoskip);

            // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.
            const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
            auto expCtx = makeExpressionContext(opCtx, *qr);
            auto cq = uassertStatusOK(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(qr),
                                             expCtx,
                                             extensionsCallback,
                                             MatchExpressionParser::kAllowAllSpecialFeatures));

            if (ctx->getView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                ctx.reset();

                // Convert the find command into an aggregation using $match (and other stages, as
                // necessary), if possible.
                const auto& qr = cq->getQueryRequest();
                auto viewAggregationCommand = uassertStatusOK(qr.asAggregationCommand());

                BSONObj aggResult = CommandHelpers::runCommandDirectly(
                    opCtx, OpMsgRequest::fromDBAndBody(_dbName, std::move(viewAggregationCommand)));
                auto status = getStatusFromCommandResult(aggResult);
                if (status.code() == ErrorCodes::InvalidPipelineOperator) {
                    uasserted(ErrorCodes::InvalidPipelineOperator,
                              str::stream() << "Unsupported in view pipeline: " << status.reason());
                }
                uassertStatusOK(status);
                result->getBodyBuilder().appendElements(aggResult);
                return;
            }

            Collection* const collection = ctx->getCollection();

            if (cq->getQueryRequest().isReadOnce()) {
                // The readOnce option causes any storage-layer cursors created during plan
                // execution to assume read data will not be needed again and need not be cached.
                opCtx->recoveryUnit()->setReadOnce(true);
            }

            // Get the execution plan for the query.
            bool permitYield = true;
            auto exec =
                uassertStatusOK(getExecutorFind(opCtx, collection, std::move(cq), permitYield));

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
                auto bodyBuilder = result->getBodyBuilder();
                appendCursorResponseObject(cursorId, nss.ns(), BSONArray(), &bodyBuilder);
                return;
            }

            FindCommon::waitInFindBeforeMakingBatch(opCtx, *exec->getCanonicalQuery());

            const QueryRequest& originalQR = exec->getCanonicalQuery()->getQueryRequest();

            // Stream query results, adding them to a BSONArray as we go.
            CursorResponseBuilder::Options options;
            options.isInitialResponse = true;
            CursorResponseBuilder firstBatch(result, options);
            BSONObj obj;
            PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
            std::uint64_t numResults = 0;
            while (!FindCommon::enoughForFirstBatch(originalQR, numResults) &&
                   PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
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
            if (PlanExecutor::FAILURE == state) {
                firstBatch.abandon();

                // We should always have a valid status member object at this point.
                auto status = WorkingSetCommon::getMemberObjectStatus(obj);
                invariant(!status.isOK());
                warning() << "Plan executor error during find command: "
                          << PlanExecutor::statestr(state) << ", status: " << status
                          << ", stats: " << redact(Explain::getWinningPlanStats(exec.get()));

                uassertStatusOK(status.withContext("Executor error during find command"));
            }

            // Set up the cursor for getMore.
            CursorId cursorId = 0;
            if (shouldSaveCursor(opCtx, collection, state, exec.get())) {
                // Create a ClientCursor containing this plan executor and register it with the
                // cursor manager.
                ClientCursorPin pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                    opCtx,
                    {std::move(exec),
                     nss,
                     AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                     opCtx->getWriteConcern(),
                     repl::ReadConcernArgs::get(opCtx),
                     _request.body,
                     ClientCursorParams::LockPolicy::kLockExternally,
                     {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)}});
                cursorId = pinnedCursor.getCursor()->cursorid();

                invariant(!exec);
                PlanExecutor* cursorExec = pinnedCursor.getCursor()->getExecutor();

                // State will be restored on getMore.
                cursorExec->saveState();
                cursorExec->detachFromOperationContext();

                // We assume that cursors created through a DBDirectClient are always used from
                // their original OperationContext, so we do not need to move time to and from the
                // cursor.
                if (!opCtx->getClient()->isInDirectClient()) {
                    pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(
                        opCtx->getRemainingMaxTimeMicros());
                }
                pinnedCursor.getCursor()->setNReturnedSoFar(numResults);
                pinnedCursor.getCursor()->incNBatches();

                // Fill out curop based on the results.
                endQueryOp(opCtx, collection, *cursorExec, numResults, cursorId);
            } else {
                endQueryOp(opCtx, collection, *exec, numResults, cursorId);
            }

            // Generate the response object to send to the client.
            firstBatch.done(cursorId, nss.ns());
        }

    private:
        const OpMsgRequest& _request;
        const StringData _dbName;
    };

} findCmd;

}  // namespace
}  // namespace mongo
