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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/acquire_locks.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_in_use_info.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/rewrite_state_change_errors.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {


MONGO_FAIL_POINT_DEFINE(rsStopGetMoreCmd);
MONGO_FAIL_POINT_DEFINE(getMoreHangAfterPinCursor);

// The timeout when waiting for linearizable read concern on a getMore command.
static constexpr Milliseconds kLinearizableReadConcernTimeout{15000};

// getMore can run with any readConcern, because cursor-creating commands like find can run with any
// readConcern.  However, since getMore automatically uses the readConcern of the command that
// created the cursor, it is not appropriate to apply the default readConcern (just as
// client-specified readConcern isn't appropriate).
static const ReadConcernSupportResult kSupportsReadConcernResult{
    Status::OK(),
    {{ErrorCodes::InvalidOptions,
      "default read concern not permitted (getMore uses the cursor's read concern)"}}};

/**
 * Validates that the lsid of 'opCtx' matches that of 'cursor'. This must be called after
 * authenticating, so that it is safe to report the lsid of 'cursor'.
 */
void validateLSID(OperationContext* opCtx, int64_t cursorId, const ClientCursor* cursor) {
    uassert(50736,
            str::stream() << "Cannot run getMore on cursor " << cursorId
                          << ", which was not created in a session, in session "
                          << *opCtx->getLogicalSessionId(),
            !opCtx->getLogicalSessionId() || cursor->getSessionId());

    uassert(50737,
            str::stream() << "Cannot run getMore on cursor " << cursorId
                          << ", which was created in session " << *cursor->getSessionId()
                          << ", without an lsid",
            opCtx->getLogicalSessionId() || !cursor->getSessionId());

    uassert(50738,
            str::stream() << "Cannot run getMore on cursor " << cursorId
                          << ", which was created in session " << *cursor->getSessionId()
                          << ", in session " << *opCtx->getLogicalSessionId(),
            !opCtx->getLogicalSessionId() || !cursor->getSessionId() ||
                (opCtx->getLogicalSessionId() == cursor->getSessionId()));
}

/**
 * Validates that the txnNumber of 'opCtx' matches that of 'cursor'. This must be called after
 * authenticating, so that it is safe to report the txnNumber of 'cursor'.
 */
void validateTxnNumber(OperationContext* opCtx, int64_t cursorId, const ClientCursor* cursor) {
    // TODO SERVER-92480 The txnNumber is not unset from the opCtx when the session is yielded, so
    // it's possible for the txnNumber to still exist despite not running in a txn. Once we unset
    // txn info from the opCtx after yielding a session, this check can be removed.
    if (!cursor->getTxnNumber() && !TransactionParticipant::get(opCtx)) {
        return;
    }

    uassert(50739,
            str::stream() << "Cannot run getMore on cursor " << cursorId
                          << ", which was not created in a transaction, in transaction "
                          << *opCtx->getTxnNumber(),
            !opCtx->getTxnNumber() || cursor->getTxnNumber());

    uassert(50740,
            str::stream() << "Cannot run getMore on cursor " << cursorId
                          << ", which was created in transaction " << *cursor->getTxnNumber()
                          << ", without a txnNumber",
            opCtx->getTxnNumber() || !cursor->getTxnNumber());

    uassert(50741,
            str::stream() << "Cannot run getMore on cursor " << cursorId
                          << ", which was created in transaction " << *cursor->getTxnNumber()
                          << ", in transaction " << *opCtx->getTxnNumber(),
            !opCtx->getTxnNumber() || !cursor->getTxnNumber() ||
                (*opCtx->getTxnNumber() == *cursor->getTxnNumber()));
}

/**
 * Validate that the client has necessary privileges to call getMore on the given cursor.
 */
void validateAuthorization(const OperationContext* opCtx, const ClientCursor& cursor) {

    auto authzSession = AuthorizationSession::get(opCtx->getClient());
    // A user can only call getMore on their own cursor.
    if (!authzSession->isCoauthorizedWith(cursor.getAuthenticatedUser())) {
        uasserted(ErrorCodes::Unauthorized,
                  str::stream() << "cursor id " << cursor.cursorid()
                                << " was not created by the authenticated user");
    }

    // Ensure that the client still has the privileges to run the originating command.
    if (!authzSession->isAuthorizedForPrivileges(cursor.getOriginatingPrivileges())) {
        uasserted(ErrorCodes::Unauthorized,
                  str::stream() << "not authorized for getMore with cursor id "
                                << cursor.cursorid());
    }
}

/**
 * Validate that the command's and cursor's namespaces match.
 */
void validateNamespace(const NamespaceString& commandNss, const ClientCursor& cursor) {
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Requested getMore on namespace '" << commandNss.toStringForErrorMsg()
                          << "', but cursor belongs to a different namespace "
                          << cursor.nss().toStringForErrorMsg(),
            commandNss == cursor.nss());

    if (commandNss.isOplog() && MONGO_unlikely(rsStopGetMoreCmd.shouldFail())) {
        uasserted(ErrorCodes::CommandFailed,
                  str::stream() << "getMore on " << commandNss.toStringForErrorMsg()
                                << " rejected due to active fail point rsStopGetMoreCmd");
    }
}

/**
 * Validate that the command's maxTimeMS is only set when the cursor is in awaitData mode.
 */
void validateMaxTimeMS(const boost::optional<std::int64_t>& commandMaxTimeMS,
                       const ClientCursor& cursor) {
    if (commandMaxTimeMS.has_value()) {
        uassert(ErrorCodes::BadValue,
                "cannot set maxTimeMS on getMore command for a non-awaitData cursor",
                cursor.isAwaitData());
    }
}

/**
 * Sets a deadline on the operation if the originating command had a maxTimeMS specified or if this
 * is a tailable, awaitData cursor.
 */
void setUpOperationDeadline(OperationContext* opCtx,
                            const ClientCursor& cursor,
                            const GetMoreCommandRequest& cmd,
                            bool disableAwaitDataFailpointActive) {

    // We assume that cursors created through a DBDirectClient are always used from their
    // original OperationContext, so we do not need to move time to and from the cursor.
    if (!opCtx->getClient()->isInDirectClient()) {
        // There is no time limit set directly on this getMore command. If the cursor is
        // awaitData, then we supply a default time of one second. Otherwise we roll over
        // any leftover time from the maxTimeMS of the operation that spawned this cursor,
        // applying it to this getMore.
        if (cursor.isAwaitData() && !disableAwaitDataFailpointActive) {
            awaitDataState(opCtx).waitForInsertsDeadline =
                opCtx->getServiceContext()->getPreciseClockSource()->now() +
                Milliseconds{cmd.getMaxTimeMS().value_or(1000)};
        } else if (cursor.getLeftoverMaxTimeMicros() < Microseconds::max()) {
            opCtx->setDeadlineAfterNowBy(cursor.getLeftoverMaxTimeMicros(),
                                         ErrorCodes::MaxTimeMSExpired);
        }
    }
}
/**
 * Sets up the OperationContext in order to correctly inherit options like the read concern from the
 * cursor to this operation.
 */
void setUpOperationContextAndCurOpStateForGetMore(OperationContext* opCtx,
                                                  CurOp* curOp,
                                                  const ClientCursor& cursor,
                                                  const GetMoreCommandRequest& cmd,
                                                  bool disableAwaitDataFailpointActive) {
    applyConcernsAndReadPreference(opCtx, cursor);

    auto apiParamsFromClient = APIParameters::get(opCtx);
    uassert(
        ErrorCodes::APIMismatchError,
        fmt::format(
            "API parameter mismatch: getMore used params {}, the cursor-creating command used {}",
            apiParamsFromClient.toBSON().toString(),
            cursor.getAPIParameters().toBSON().toString()),
        apiParamsFromClient == cursor.getAPIParameters());

    setUpOperationDeadline(opCtx, cursor, cmd, disableAwaitDataFailpointActive);

    auto originatingCommand = cursor.getOriginatingCommandObj();
    if (!originatingCommand.isEmpty()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());

        // Ensure that the original query or command object is available in the slow query log,
        // profiler and currentOp.
        curOp->setOriginatingCommand(lk, originatingCommand);

        // If the originating command had a 'comment' field, we extract it and set it on opCtx. Note
        // that if the 'getMore' command itself has a 'comment' field, we give precedence to it.
        auto comment = originatingCommand["comment"];

        if (!opCtx->getComment() && comment) {
            opCtx->setComment(comment.wrap());
        }
    }
}

/**
 * A command for running getMore() against an existing cursor registered with a CursorManager.
 * Used to generate the next batch of results for a ClientCursor.
 *
 * Can be used in combination with any cursor-generating command (e.g. find, aggregate,
 * listIndexes).
 */
class GetMoreCmd final : public Command {
public:
    GetMoreCmd() : Command("getMore") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        return std::make_unique<Invocation>(this, opMsgRequest);
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd, const OpMsgRequest& request)
            : CommandInvocation(cmd),
              _cmd(GetMoreCommandRequest::parse(request, IDLParserContext{"getMore"})) {
            NamespaceString nss(
                NamespaceStringUtil::deserialize(_cmd.getDbName(), _cmd.getCollection()));
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace for getMore: " << nss.toStringForErrorMsg(),
                    nss.isValid());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return kSupportsReadConcernResult;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return !_cmd.getTerm().has_value();
        }

        bool allowsAfterClusterTime() const override {
            return false;
        }

        bool canIgnorePrepareConflicts() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceStringUtil::deserialize(_cmd.getDbName(), _cmd.getCollection());
        }

        const DatabaseName& db() const override {
            return _cmd.getDbName();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(auth::checkAuthForGetMore(AuthorizationSession::get(opCtx->getClient()),
                                                      ns(),
                                                      _cmd.getCommandParameter(),
                                                      _cmd.getTerm().has_value()));
        }

        const GenericArguments& getGenericArguments() const override {
            return _cmd.getGenericArguments();
        }

        bool canRetryOnStaleConfigOrShardCannotRefreshDueToLocksHeld(
            const OpMsgRequest& request) const override {
            // Can not rerun the command when executing a GetMore command as the cursor may already
            // be lost.
            return false;
        }

        /**
         * Implements populating 'nextBatch' with up to 'batchSize' documents from the plan executor
         * 'exec'. Outputs the number of documents and relevant size statistics in 'numResults'.
         * Returns whether or not the cursor should be saved.
         */
        bool batchedExecute(OperationContext* opCtx,
                            ClientCursor* cursor,
                            PlanExecutor* exec,
                            const size_t batchSize,
                            const bool isTailable,
                            CursorResponseBuilder* nextBatch,
                            uint64_t* numResults) {
            BSONObj obj;
            BSONObj pbrt = exec->getPostBatchResumeToken();
            bool failedToAppend = false;

            // Capture diagnostics to be logged in the case of a failure.
            ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                               diagnostic_printers::ExplainDiagnosticPrinter{exec});

            // Note that unlike in find, a batch size of 0 means there is no limit on the number of
            // documents, and we may choose to pre-allocate space for the batch after the first
            // object.
            if (PlanExecutor::ADVANCED == exec->getNext(&obj, nullptr)) {
                // Reserve space based on the size of the first object. Note that we always
                // allow the first object to be appended to the batch so we can make
                // progress.
                size_t objSize = obj.objsize();
                auto bytesToReserve =
                    FindCommon::getBytesToReserveForGetMoreReply(isTailable, objSize, batchSize);
                nextBatch->reserveReplyBuffer(bytesToReserve);
                // Don't check document size here before appending, since we always want to make
                // progress.
                nextBatch->append(obj);
                *numResults = 1;

                // As soon as we get a result, this operation no longer waits.
                awaitDataState(opCtx).shouldWaitForInserts = false;

                // If this executor produces a postBatchResumeToken, store it so we can later add it
                // to the response.
                pbrt = exec->getPostBatchResumeToken();

                // We decrease the batch size by 1 to account for the document we just emitted via
                // the first call to getNext() above.
                *numResults += exec->getNextBatch(
                    batchSize - 1,
                    FindCommon::BSONObjCursorAppender{
                        false /* alwaysAcceptFirstDoc */, nextBatch, pbrt, failedToAppend});
            }

            // Use the resume token generated by the last execution of the plan that didn't stash a
            // document, or the latest resume token if we hit EOF/the end of the batch.
            nextBatch->setPostBatchResumeToken(failedToAppend ? pbrt
                                                              : exec->getPostBatchResumeToken());
            return shouldSaveCursorGetMore(exec, isTailable);
        }

        /**
         * Uses 'cursor' and 'request' to fill out 'nextBatch' with the batch of result documents to
         * be returned by this getMore.
         *
         * Returns true if the cursor should be saved for subsequent getMores, and false otherwise.
         * Fills out *numResults with the number of documents in the batch, which must be
         * initialized to zero by the caller.
         *
         * Throws an exception on failure.
         */
        bool generateBatch(OperationContext* opCtx,
                           ClientCursor* cursor,
                           const GetMoreCommandRequest& cmd,
                           const bool isTailable,
                           CursorResponseBuilder* nextBatch,
                           uint64_t* numResults) {
            // Note: if an awaitData getMore is killed during this process due to our max time
            // expiring at an interrupt point, we just continue as normal and return rather than
            // reporting a timeout to the user.
            PlanExecutor* exec = cursor->getExecutor();

            // We intentionally set the batch size to the max size_t value for a batch size of 0 in
            // order to simulate "no limit" on the batch size. We will run out of space in the
            // buffer before we reach this limit anyway.
            size_t batchSize = cmd.getBatchSize().value_or(std::numeric_limits<size_t>::max());

            try {
                return batchedExecute(
                    opCtx, cursor, exec, batchSize, isTailable, nextBatch, numResults);
            } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
                // This exception indicates that we should close the cursor without reporting an
                // error.
                return false;
            } catch (const ExceptionFor<ErrorCodes::ChangeStreamInvalidated>& ex) {
                // This exception is thrown when a change-stream cursor is invalidated. Set the PBRT
                // to the resume token of the invalidating event, and mark the cursor response as
                // invalidated. We always expect to have ExtraInfo for this error code.
                const auto extraInfo = ex.extraInfo<ChangeStreamInvalidationInfo>();
                tassert(5493700, "Missing ChangeStreamInvalidationInfo on exception", extraInfo);

                nextBatch->setPostBatchResumeToken(extraInfo->getInvalidateResumeToken());
                nextBatch->setInvalidated();
                return false;
            } catch (DBException& exception) {
                nextBatch->abandon();

                auto&& explainer = exec->getPlanExplainer();
                auto&& [stats, _] =
                    explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
                LOGV2_WARNING(20478,
                              "getMore command executor error",
                              "error"_attr = exception.toStatus(),
                              "stats"_attr = redact(stats),
                              "cmd"_attr = redact(cmd.toBSON()));

                exception.addContext("Executor error during getMore");
                throw;
            }
        }

        void acquireLocksAndIterateCursor(OperationContext* opCtx,
                                          rpc::ReplyBuilderInterface* reply,
                                          ClientCursorPin& cursorPin,
                                          CurOp* curOp) {
            const bool disableAwaitDataFailpointActive =
                MONGO_unlikely(disableAwaitDataForGetMoreCmd.shouldFail());

            // Inherit properties like readConcern and maxTimeMS from our originating cursor.
            setUpOperationContextAndCurOpStateForGetMore(
                opCtx, curOp, *cursorPin.getCursor(), _cmd, disableAwaitDataFailpointActive);

            NamespaceString nss = ns();
            CursorLocks locks{opCtx, nss, cursorPin};

            // On early return, typically due to a failed assertion, delete the cursor.
            ScopeGuard cursorDeleter([&] {
                cursorPin.deleteUnderlying();
                if (locks.txnResourcesHandler) {
                    locks.txnResourcesHandler->dismissRestoredResources();
                }
            });

            if (MONGO_unlikely(waitAfterPinningCursorBeforeGetMoreBatch.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &waitAfterPinningCursorBeforeGetMoreBatch,
                    opCtx,
                    "waitAfterPinningCursorBeforeGetMoreBatch",
                    []() {}, /*empty function*/
                    nss);
            }

            if (!cursorPin->isAwaitData()) {
                opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.
            }

            PlanExecutor* exec = cursorPin->getExecutor();
            const auto* cq = exec->getCanonicalQuery();
            if (cq && cq->getFindCommandRequest().getReadOnce()) {
                // The readOnce option causes any storage-layer cursors created during plan
                // execution to assume read data will not be needed again and need not be cached.
                shard_role_details::getRecoveryUnit(opCtx)->setReadOnce(true);
            }
            exec->reattachToOperationContext(opCtx);
            exec->restoreState(locks.readLock ? &locks.readLock->getCollection() : nullptr);

            {
                auto planSummary = exec->getPlanExplainer().getPlanSummary();

                stdx::lock_guard<Client> lk(*opCtx->getClient());
                curOp->setPlanSummary(lk, std::move(planSummary));

                curOp->debug().queryFramework = exec->getQueryFramework();
                curOp->setShouldOmitDiagnosticInformation(
                    lk, cursorPin->shouldOmitDiagnosticInformation());

                // Update the genericCursor stored in curOp with the new cursor stats.
                curOp->setGenericCursor(lk, cursorPin->toGenericCursor());
            }

            // If the 'failGetMoreAfterCursorCheckout' failpoint is enabled, throw an exception with
            // the given 'errorCode' value, or ErrorCodes::InternalError if 'errorCode' is omitted.
            failGetMoreAfterCursorCheckout.executeIf(
                [&](const BSONObj& data) {
                    rpc::RewriteStateChangeErrors::onActiveFailCommand(opCtx, data);
                    auto errorCode = (data["errorCode"] ? data["errorCode"].safeNumberLong()
                                                        : ErrorCodes::InternalError);
                    uasserted(errorCode, "Hit the 'failGetMoreAfterCursorCheckout' failpoint");
                },
                [&opCtx, &cursorPin](const BSONObj& data) {
                    auto dataForFailCommand =
                        data.addField(BSON("failCommands" << BSON_ARRAY("getMore")).firstElement());
                    auto* getMoreCommand = CommandHelpers::findCommand(opCtx, "getMore");
                    return CommandHelpers::shouldActivateFailCommandFailPoint(
                        dataForFailCommand, cursorPin->nss(), getMoreCommand, opCtx->getClient());
                });

            CursorId respondWithId = 0;
            CursorResponseBuilder::Options options;
            if (!opCtx->inMultiDocumentTransaction()) {
                options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
            }
            CursorResponseBuilder nextBatch(reply, options);
            uint64_t numResults = 0;

            // We report keysExamined and docsExamined to OpDebug for a given getMore operation. To
            // obtain these values we need to take a diff of the pre-execution and post-execution
            // metrics, as they accumulate over the course of a cursor's lifetime.
            PlanSummaryStats preExecutionStats;
            exec->getPlanExplainer().getSummaryStats(&preExecutionStats);

            // Mark this as an AwaitData operation if appropriate.
            if (cursorPin->isAwaitData() && !disableAwaitDataFailpointActive) {
                auto lastKnownCommittedOpTime = _cmd.getLastKnownCommittedOpTime();
                if (opCtx->isExhaust() && cursorPin->getLastKnownCommittedOpTime()) {
                    // Use the commit point of the last batch for exhaust cursors.
                    lastKnownCommittedOpTime = cursorPin->getLastKnownCommittedOpTime();
                }
                clientsLastKnownCommittedOpTime(opCtx) = lastKnownCommittedOpTime;

                awaitDataState(opCtx).shouldWaitForInserts = true;
            }

            waitWithPinnedCursorDuringGetMoreBatch.execute([&](const BSONObj& data) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &waitWithPinnedCursorDuringGetMoreBatch,
                    opCtx,
                    "waitWithPinnedCursorDuringGetMoreBatch",
                    []() {}, /*empty function*/
                    nss);
            });

            const auto shouldSaveCursor = generateBatch(opCtx,
                                                        cursorPin.getCursor(),
                                                        _cmd,
                                                        cursorPin->isTailable(),
                                                        &nextBatch,
                                                        &numResults);

            PlanSummaryStats postExecutionStats;
            exec->getPlanExplainer().getSummaryStats(&postExecutionStats);
            postExecutionStats.totalKeysExamined -= preExecutionStats.totalKeysExamined;
            postExecutionStats.totalDocsExamined -= preExecutionStats.totalDocsExamined;
            curOp->debug().setPlanSummaryMetrics(std::move(postExecutionStats));

            // We do not report 'execStats' for aggregation or other cursors with the
            // 'kLocksInternally' policy, both in the original request and subsequent getMore. It
            // would be useful to have this info for an aggregation, but the source PlanExecutor
            // could be destroyed before we know if we need 'execStats' and we do not want to
            // generate the stats eagerly for all operations due to cost.
            if (cursorPin->getExecutor()->lockPolicy() !=
                    PlanExecutor::LockPolicy::kLocksInternally &&
                curOp->shouldDBProfile()) {
                auto&& explainer = exec->getPlanExplainer();
                auto&& [stats, _] =
                    explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
                curOp->debug().execStats = std::move(stats);
            }

            if (shouldSaveCursor) {
                respondWithId = cursorPin->cursorid();

                exec->saveState();
                exec->detachFromOperationContext();

                cursorPin->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

                if (opCtx->isExhaust() && clientsLastKnownCommittedOpTime(opCtx)) {
                    // Update the cursor's lastKnownCommittedOpTime to the current
                    // lastCommittedOpTime. The lastCommittedOpTime now may be staler than the
                    // actual lastCommittedOpTime returned in the metadata of this latest batch (see
                    // appendReplyMetadata).  As a result, we may sometimes return more empty
                    // batches than we need to. But it is fine to be conservative in this.
                    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
                    auto myLastCommittedOpTime = replCoord->getLastCommittedOpTime();
                    auto clientsLastKnownCommittedOpTime = cursorPin->getLastKnownCommittedOpTime();
                    if (!clientsLastKnownCommittedOpTime.has_value() ||
                        clientsLastKnownCommittedOpTime.value() < myLastCommittedOpTime) {
                        cursorPin->setLastKnownCommittedOpTime(myLastCommittedOpTime);
                    }
                }
            } else {
                curOp->debug().cursorExhausted = true;
            }

            // Collect and increment metrics now that we have enough information. It's important
            // we do so before generating the response so that the response can include metrics.
            curOp->debug().additiveMetrics.nBatches = 1;
            curOp->setEndOfOpMetrics(numResults);
            collectQueryStatsMongod(opCtx, cursorPin);

            boost::optional<CursorMetrics> metrics = _cmd.getIncludeQueryStatsMetrics()
                ? boost::make_optional(CurOp::get(opCtx)->debug().getCursorMetrics())
                : boost::none;
            nextBatch.done(respondWithId,
                           nss,
                           metrics,
                           SerializationContext::stateCommandReply(_cmd.getSerializationContext()));

            if (respondWithId) {
                cursorDeleter.dismiss();

                if (opCtx->isExhaust()) {
                    // Indicate that an exhaust message should be generated and the previous BSONObj
                    // command parameters should be reused as the next BSONObj command parameters.
                    reply->setNextInvocation(boost::none);
                }
            }
        }

        ClientCursorPin pinCursorWithRetry(OperationContext* opCtx,
                                           CursorId cursorId,
                                           const NamespaceString& nss) {
            // Perform validation checks which don't cause the cursor to be deleted on failure.
            auto pinCheck = [&](const ClientCursor& cc) {
                validateLSID(opCtx, cursorId, &cc);
                validateTxnNumber(opCtx, cursorId, &cc);
                validateAuthorization(opCtx, cc);
                validateNamespace(nss, cc);
                validateMaxTimeMS(_cmd.getMaxTimeMS(), cc);
            };

            Backoff retryBackoff{Seconds(1), Milliseconds::max()};

            const size_t maxAttempts = internalQueryGetMoreMaxCursorPinRetryAttempts.loadRelaxed();
            for (size_t attempt = 1; attempt <= maxAttempts; ++attempt) {
                auto statusWithCursorPin = CursorManager::get(opCtx)->pinCursor(
                    opCtx, cursorId, definition()->getName(), pinCheck);
                auto status = statusWithCursorPin.getStatus();
                if (status.isOK()) {
                    return std::move(statusWithCursorPin.getValue());
                }
                // We only return CursorInUse errors if the command that is holding the cursor is
                // "releaseMemory".
                if (attempt == maxAttempts || status.code() != ErrorCodes::CursorInUse) {
                    uassertStatusOK(status);
                }
                auto extraInfo = status.extraInfo<CursorInUseInfo>();
                if (extraInfo == nullptr || extraInfo->commandName() != "releaseMemory") {
                    uassertStatusOK(status);
                }
                Milliseconds sleepDuration = retryBackoff.nextSleep();
                LOGV2_DEBUG(10116501,
                            3,
                            "getMore failed to pin cursor, because it is held by releaseMemory "
                            "command. Will retry after sleep.",
                            "sleepDuration"_attr = sleepDuration,
                            "attempt"_attr = attempt);
                opCtx->sleepFor(sleepDuration);
            }
            MONGO_UNREACHABLE_TASSERT(101165);
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            // Gets the number of write ops in the current multidocument transaction.
            auto getNumTxnOps = [opCtx]() -> boost::optional<size_t> {
                if (opCtx->inMultiDocumentTransaction()) {
                    auto participant = TransactionParticipant::get(opCtx);
                    if (participant) {
                        return participant.getTransactionOperationsCount();
                    }
                }
                return boost::optional<size_t>{};
            };

            // Enforces that getMore does not perform any write ops when executing in a transaction.
            auto invariantIfHasDoneWrites = [&getNumTxnOps, opCtx](boost::optional<size_t> numPre) {
                if (numPre) {
                    boost::optional<size_t> numTxnOps = getNumTxnOps();
                    invariant(numPre == numTxnOps);
                }
            };

            // Counted as a getMore, not as a command.
            serviceOpCounters(opCtx).gotGetMore();
            auto curOp = CurOp::get(opCtx);
            NamespaceString nss = ns();
            int64_t cursorId = _cmd.getCommandParameter();
            curOp->debug().cursorid = cursorId;

            // The presence of a term in the request indicates that this is an internal replication
            // oplog read request.
            boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> admissionPriority;
            if (_cmd.getTerm() && nss == NamespaceString::kRsOplogNamespace) {
                // Validate term before acquiring locks.
                auto replCoord = repl::ReplicationCoordinator::get(opCtx);
                // Note: updateTerm returns ok if term stayed the same.
                uassertStatusOK(replCoord->updateTerm(opCtx, *_cmd.getTerm()));

                // If the term field is present in an oplog request, it means this is an oplog
                // getMore for replication oplog fetching because the term field is only allowed for
                // internal clients (see checkAuthForGetMore).
                curOp->debug().isReplOplogGetMore = true;

                // We do not want to wait to take tickets for internal (replication) oplog reads.
                // Stalling on ticket acquisition can cause complicated deadlocks. Primaries may
                // depend on data reaching secondaries in order to proceed; and secondaries may get
                // stalled replicating because of an inability to acquire a read ticket.
                admissionPriority.emplace(opCtx, AdmissionContext::Priority::kExempt);
            }

            if (_cmd.getIncludeQueryStatsMetrics()) {
                curOp->debug().queryStatsInfo.metricsRequested = true;
            }

            ClientCursorPin cursorPin = pinCursorWithRetry(opCtx, cursorId, nss);

            // Get the read concern level here in case the cursor is exhausted while iterating.
            const auto isLinearizableReadConcern = cursorPin->getReadConcernArgs().getLevel() ==
                repl::ReadConcernLevel::kLinearizableReadConcern;

            // If in a multi-document transaction, save the size of transactionOperations for
            // later checking of whether this invocation performed a write.
            boost::optional<size_t> numTxnOpsPre = getNumTxnOps();

            acquireLocksAndIterateCursor(opCtx, reply, cursorPin, curOp);

            if (MONGO_unlikely(getMoreHangAfterPinCursor.shouldFail())) {
                LOGV2(20477,
                      "getMoreHangAfterPinCursor fail point enabled. Blocking until fail "
                      "point is disabled",
                      "cursorId"_attr = cursorId);
                getMoreHangAfterPinCursor.pauseWhileSet(opCtx);
            }

            if (isLinearizableReadConcern) {
                // waitForLinearizableReadConcern performs a NoOp write and waits for that write
                // to have been majority committed. awaitReplication requires that we release all
                // locks to prevent blocking for a long time while doing network activity. Since
                // getMores do not have support for a maxTimeout duration, we hardcode the timeout
                // to avoid waiting indefinitely.
                uassertStatusOK(
                    mongo::waitForLinearizableReadConcern(opCtx, kLinearizableReadConcernTimeout));
            }

            // We're about to unpin or delete the cursor as the ClientCursorPin goes out of scope.
            // If the 'waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch' failpoint is active, we
            // set the 'msg' field of this operation's CurOp to signal that we've hit this point and
            // then spin until the failpoint is released.
            if (MONGO_unlikely(waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch,
                    opCtx,
                    "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch");
            }

            // getMore must not write if running inside a multi-document transaction.
            invariantIfHasDoneWrites(numTxnOpsPre);

            if (getTestCommandsEnabled()) {
                validateResult(opCtx, reply, nss.tenantId());
            }
        }

        void validateResult(OperationContext* opCtx,
                            rpc::ReplyBuilderInterface* reply,
                            boost::optional<TenantId> tenantId) {
            auto ret = reply->getBodyBuilder().asTempObj();

            // We need to copy the serialization context from the request to the reply object
            CursorGetMoreReply::parse(ret.removeField("ok"),
                                      IDLParserContext("CursorGetMoreReply",
                                                       auth::ValidatedTenancyScope::get(opCtx),
                                                       tenantId,
                                                       SerializationContext::stateCommandReply(
                                                           _cmd.getSerializationContext())));
        }

        const GetMoreCommandRequest _cmd;
    };

    bool maintenanceOk() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::string help() const override {
        return "retrieve more results from an existing cursor";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opGetMore;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    /**
     * A getMore command increments the getMore counter, not the command counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }
};
MONGO_REGISTER_COMMAND(GetMoreCmd).forShard();

}  // namespace
}  // namespace mongo
