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

#include <fmt/format.h>
#include <string>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/external_data_source_scope_guard.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/rewrite_state_change_errors.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

using namespace fmt::literals;

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
            str::stream() << "Requested getMore on namespace '" << commandNss.ns()
                          << "', but cursor belongs to a different namespace " << cursor.nss().ns(),
            commandNss == cursor.nss());

    if (commandNss.isOplog() && MONGO_unlikely(rsStopGetMoreCmd.shouldFail())) {
        uasserted(ErrorCodes::CommandFailed,
                  str::stream() << "getMore on " << commandNss.ns()
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
 * Apply the read concern from the cursor to this operation.
 */
void applyCursorReadConcern(OperationContext* opCtx, repl::ReadConcernArgs rcArgs) {
    const auto replicationMode = repl::ReplicationCoordinator::get(opCtx)->getReplicationMode();

    // Select the appropriate read source. If we are in a transaction with read concern majority,
    // this will already be set to kNoTimestamp, so don't set it again.
    if (replicationMode == repl::ReplicationCoordinator::modeReplSet &&
        rcArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern &&
        !opCtx->inMultiDocumentTransaction()) {
        switch (rcArgs.getMajorityReadMechanism()) {
            case repl::ReadConcernArgs::MajorityReadMechanism::kMajoritySnapshot: {
                // Make sure we read from the majority snapshot.
                opCtx->recoveryUnit()->setTimestampReadSource(
                    RecoveryUnit::ReadSource::kMajorityCommitted);
                uassertStatusOK(opCtx->recoveryUnit()->majorityCommittedSnapshotAvailable());
                break;
            }
            case repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative: {
                // Mark the operation as speculative and select the correct read source.
                repl::SpeculativeMajorityReadInfo::get(opCtx).setIsSpeculativeRead();
                opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
                break;
            }
        }
    }

    if (replicationMode == repl::ReplicationCoordinator::modeReplSet &&
        rcArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern &&
        !opCtx->inMultiDocumentTransaction()) {
        auto atClusterTime = rcArgs.getArgsAtClusterTime();
        invariant(atClusterTime && *atClusterTime != LogicalTime::kUninitialized);
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                      atClusterTime->asTimestamp());
    }

    // For cursor commands that take locks internally, the read concern on the
    // OperationContext may affect the timestamp read source selected by the storage engine.
    // We place the cursor read concern onto the OperationContext so the lock acquisition
    // respects the cursor's read concern.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        repl::ReadConcernArgs::get(opCtx) = rcArgs;
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
void setUpOperationContextStateForGetMore(OperationContext* opCtx,
                                          const ClientCursor& cursor,
                                          const GetMoreCommandRequest& cmd,
                                          bool disableAwaitDataFailpointActive) {
    applyCursorReadConcern(opCtx, cursor.getReadConcernArgs());
    opCtx->setWriteConcern(cursor.getWriteConcernOptions());
    ReadPreferenceSetting::get(opCtx) = cursor.getReadPreferenceSetting();

    auto apiParamsFromClient = APIParameters::get(opCtx);
    uassert(
        ErrorCodes::APIMismatchError,
        "API parameter mismatch: getMore used params {}, the cursor-creating command used {}"_format(
            apiParamsFromClient.toBSON().toString(), cursor.getAPIParameters().toBSON().toString()),
        apiParamsFromClient == cursor.getAPIParameters());

    setUpOperationDeadline(opCtx, cursor, cmd, disableAwaitDataFailpointActive);

    // If the originating command had a 'comment' field, we extract it and set it on opCtx. Note
    // that if the 'getMore' command itself has a 'comment' field, we give precedence to it.
    auto comment = cursor.getOriginatingCommandObj()["comment"];
    if (!opCtx->getComment() && comment) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->setComment(comment.wrap());
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

    const std::set<std::string>& apiVersions() const {
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
    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd, const OpMsgRequest& request)
            : CommandInvocation(cmd),
              _cmd(GetMoreCommandRequest::parse(IDLParserContext{"getMore"}, request)) {
            NamespaceString nss(NamespaceStringUtil::parseNamespaceFromRequest(
                _cmd.getDbName(), _cmd.getCollection()));
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace for getMore: " << nss.ns(),
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

        bool allowsAfterClusterTime() const override {
            return false;
        }

        bool canIgnorePrepareConflicts() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceStringUtil::parseNamespaceFromRequest(_cmd.getDbName(),
                                                                  _cmd.getCollection());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(auth::checkAuthForGetMore(AuthorizationSession::get(opCtx->getClient()),
                                                      ns(),
                                                      _cmd.getCommandParameter(),
                                                      _cmd.getTerm().has_value()));
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
                           std::uint64_t* numResults,
                           ResourceConsumption::DocumentUnitCounter* docUnitsReturned) {
            PlanExecutor* exec = cursor->getExecutor();

            // If an awaitData getMore is killed during this process due to our max time expiring at
            // an interrupt point, we just continue as normal and return rather than reporting a
            // timeout to the user.
            BSONObj obj;
            PlanExecutor::ExecState state;
            size_t batchSize = cmd.getBatchSize().value_or(0);
            try {
                while (!FindCommon::enoughForGetMore(batchSize, *numResults) &&
                       PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
                    // If adding this object will cause us to exceed the message size limit, then we
                    // stash it for later.
                    if (!FindCommon::haveSpaceForNext(obj, *numResults, nextBatch->bytesUsed())) {
                        exec->stashResult(obj);
                        break;
                    }

                    // As soon as we get a result, this operation no longer waits.
                    awaitDataState(opCtx).shouldWaitForInserts = false;

                    // If this executor produces a postBatchResumeToken, add it to the response.
                    nextBatch->setPostBatchResumeToken(exec->getPostBatchResumeToken());

                    // At this point, we know that there will be at least one document in this
                    // batch. Reserve an initial estimated number of bytes for the response.
                    if (*numResults == 0) {
                        auto bytesToReserve = FindCommon::getBytesToReserveForGetMoreReply(
                            isTailable, obj.objsize(), batchSize);
                        nextBatch->reserveReplyBuffer(bytesToReserve);
                    }

                    nextBatch->append(obj);
                    (*numResults)++;
                    docUnitsReturned->observeOne(obj.objsize());
                }
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
                              "getMore command executor error: {error}, stats: {stats}, cmd: {cmd}",
                              "getMore command executor error",
                              "error"_attr = exception.toStatus(),
                              "stats"_attr = redact(stats),
                              "cmd"_attr = cmd.toBSON({}));

                exception.addContext("Executor error during getMore");
                throw;
            }

            if (state == PlanExecutor::IS_EOF) {
                // The latest oplog timestamp may advance even when there are no results. Ensure
                // that we have the latest postBatchResumeToken produced by the plan executor.
                // The getMore command does not accept a batchSize of 0, so empty batches are
                // always caused by hitting EOF and do not need to be handled separately.
                nextBatch->setPostBatchResumeToken(exec->getPostBatchResumeToken());
            }

            return shouldSaveCursorGetMore(exec, isTailable);
        }

        void acquireLocksAndIterateCursor(OperationContext* opCtx,
                                          rpc::ReplyBuilderInterface* reply,
                                          ClientCursorPin& cursorPin,
                                          CurOp* curOp) {
            // Get a reference to the shared_ptr so that we drop the virtual collections (via the
            // destructor) after deleting our cursors and releasing our read locks.
            std::shared_ptr<ExternalDataSourceScopeGuard> extDataSourceScopeGuard =
                ExternalDataSourceScopeGuard::get(cursorPin.getCursor());

            // Cursors come in one of two flavors:
            //
            // - Cursors which read from a single collection, such as those generated via the
            //   find command. For these cursors, we hold the appropriate collection lock for the
            //   duration of the getMore using AutoGetCollectionForRead. These cursors have the
            //   'kLockExternally' lock policy.
            //
            // - Cursors which may read from many collections, e.g. those generated via the
            //   aggregate command, or which do not read from a collection at all, e.g. those
            //   generated by the listIndexes command. We don't need to acquire locks to use these
            //   cursors, since they either manage locking themselves or don't access data protected
            //   by collection locks. These cursors have the 'kLocksInternally' lock policy.
            //
            // While we only need to acquire locks for 'kLockExternally' cursors, we need to create
            // an AutoStatsTracker in either case. This is responsible for updating statistics in
            // CurOp and Top. We avoid using AutoGetCollectionForReadCommand because we may need to
            // drop and reacquire locks when the cursor is awaitData, but we don't want to update
            // the stats twice.
            boost::optional<AutoGetCollectionForReadMaybeLockFree> readLock;
            boost::optional<AutoStatsTracker> statsTracker;
            NamespaceString nss(NamespaceStringUtil::parseNamespaceFromRequest(
                _cmd.getDbName(), _cmd.getCollection()));

            const bool disableAwaitDataFailpointActive =
                MONGO_unlikely(disableAwaitDataForGetMoreCmd.shouldFail());

            // Inherit properties like readConcern and maxTimeMS from our originating cursor.
            setUpOperationContextStateForGetMore(
                opCtx, *cursorPin.getCursor(), _cmd, disableAwaitDataFailpointActive);

            // Update opCtx of the decorated ExternalDataSourceScopeGuard object so that it can drop
            // virtual collections in the new 'opCtx'.
            ExternalDataSourceScopeGuard::updateOperationContext(cursorPin.getCursor(), opCtx);

            // On early return, typically due to a failed assertion, delete the cursor.
            ScopeGuard cursorDeleter([&] { cursorPin.deleteUnderlying(); });

            if (cursorPin->getExecutor()->lockPolicy() ==
                PlanExecutor::LockPolicy::kLocksInternally) {
                if (!nss.isCollectionlessCursorNamespace()) {
                    statsTracker.emplace(
                        opCtx,
                        nss,
                        Top::LockType::NotLocked,
                        AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));
                }
            } else {
                invariant(cursorPin->getExecutor()->lockPolicy() ==
                          PlanExecutor::LockPolicy::kLockExternally);

                // Lock the backing collection by using the executor's namespace. Note that it may
                // be different from the cursor's namespace. One such possible scenario is when
                // getMore() is executed against a view. Technically, views are pipelines and under
                // normal circumstances use 'kLocksInternally' policy, so we shouldn't be getting
                // into here in the first place. However, if the pipeline was optimized away and
                // replaced with a query plan, its lock policy would have also been changed to
                // 'kLockExternally'. So, we'll use the executor's namespace to take the lock (which
                // is always the backing collection namespace), but will use the namespace provided
                // in the user request for profiling.
                // Otherwise, these two namespaces will match.
                // Note that some pipelines which were optimized away may require locking multiple
                // namespaces. As such, we pass any secondary namespaces required by the pinned
                // cursor's executor when constructing 'readLock'.
                readLock.emplace(opCtx,
                                 cursorPin->getExecutor()->nss(),
                                 AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                     cursorPin->getExecutor()->getSecondaryNamespaces()));

                statsTracker.emplace(
                    opCtx,
                    nss,
                    Top::LockType::ReadLocked,
                    AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                    CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));

                // Check whether we are allowed to read from this node after acquiring our locks.
                uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(
                    opCtx, nss, true));
            }

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
                opCtx->recoveryUnit()->setReadOnce(true);
            }
            exec->reattachToOperationContext(opCtx);
            exec->restoreState(readLock ? &readLock->getCollection() : nullptr);

            auto planSummary = exec->getPlanExplainer().getPlanSummary();
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                curOp->setPlanSummary_inlock(planSummary);

                // Ensure that the original query or command object is available in the slow query
                // log, profiler and currentOp.
                auto originatingCommand = cursorPin->getOriginatingCommandObj();
                if (!originatingCommand.isEmpty()) {
                    curOp->setOriginatingCommand_inlock(originatingCommand);
                }

                curOp->debug().queryFramework = exec->getQueryFramework();
                curOp->debug().shouldOmitDiagnosticInformation =
                    cursorPin->shouldOmitDiagnosticInformation();

                // Update the genericCursor stored in curOp with the new cursor stats.
                curOp->setGenericCursor_inlock(cursorPin->toGenericCursor());
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
                    auto* getMoreCommand = CommandHelpers::findCommand("getMore");
                    return CommandHelpers::shouldActivateFailCommandFailPoint(
                        dataForFailCommand, cursorPin->nss(), getMoreCommand, opCtx->getClient());
                });

            CursorId respondWithId = 0;
            CursorResponseBuilder::Options options;
            if (!opCtx->inMultiDocumentTransaction()) {
                options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
            }
            CursorResponseBuilder nextBatch(reply, options);
            std::uint64_t numResults = 0;
            ResourceConsumption::DocumentUnitCounter docUnitsReturned;

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
                if (lastKnownCommittedOpTime) {
                    clientsLastKnownCommittedOpTime(opCtx) = lastKnownCommittedOpTime.value();
                }

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
                                                        &numResults,
                                                        &docUnitsReturned);

            PlanSummaryStats postExecutionStats;
            exec->getPlanExplainer().getSummaryStats(&postExecutionStats);
            postExecutionStats.totalKeysExamined -= preExecutionStats.totalKeysExamined;
            postExecutionStats.totalDocsExamined -= preExecutionStats.totalDocsExamined;
            curOp->debug().setPlanSummaryMetrics(postExecutionStats);

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

                if (opCtx->isExhaust() && !clientsLastKnownCommittedOpTime(opCtx).isNull()) {
                    // Set the commit point of the latest batch.
                    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
                    cursorPin->setLastKnownCommittedOpTime(replCoord->getLastCommittedOpTime());
                }
            } else {
                curOp->debug().cursorExhausted = true;
            }

            nextBatch.done(respondWithId, nss);

            // Increment this metric once we have generated a response and we know it will return
            // documents.
            auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
            metricsCollector.incrementDocUnitsReturned(curOp->getNS(), docUnitsReturned);
            curOp->debug().additiveMetrics.nBatches = 1;

            collectTelemetryMongod(opCtx, cursorPin, numResults);

            if (respondWithId) {
                cursorDeleter.dismiss();

                if (opCtx->isExhaust()) {
                    // Indicate that an exhaust message should be generated and the previous BSONObj
                    // command parameters should be reused as the next BSONObj command parameters.
                    reply->setNextInvocation(boost::none);
                }
            }
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            // Counted as a getMore, not as a command.
            globalOpCounters.gotGetMore();
            auto curOp = CurOp::get(opCtx);
            NamespaceString nss = ns();
            int64_t cursorId = _cmd.getCommandParameter();
            curOp->debug().cursorid = cursorId;

            // The presence of a term in the request indicates that this is an internal replication
            // oplog read request.
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
                opCtx->lockState()->setAdmissionPriority(AdmissionContext::Priority::kImmediate);
            }

            // Perform validation checks which don't cause the cursor to be deleted on failure.
            auto pinCheck = [&](const ClientCursor& cc) {
                validateLSID(opCtx, cursorId, &cc);
                validateTxnNumber(opCtx, cursorId, &cc);
                validateAuthorization(opCtx, cc);
                validateNamespace(nss, cc);
                validateMaxTimeMS(_cmd.getMaxTimeMS(), cc);
            };

            auto cursorPin =
                uassertStatusOK(CursorManager::get(opCtx)->pinCursor(opCtx, cursorId, pinCheck));

            // Get the read concern level here in case the cursor is exhausted while iterating.
            const auto isLinearizableReadConcern = cursorPin->getReadConcernArgs().getLevel() ==
                repl::ReadConcernLevel::kLinearizableReadConcern;

            acquireLocksAndIterateCursor(opCtx, reply, cursorPin, curOp);

            if (MONGO_unlikely(getMoreHangAfterPinCursor.shouldFail())) {
                LOGV2(20477,
                      "getMoreHangAfterPinCursor fail point enabled. Blocking until fail "
                      "point is disabled");
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

            if (getTestCommandsEnabled()) {
                validateResult(reply, nss.tenantId());
            }
        }

        void validateResult(rpc::ReplyBuilderInterface* reply, boost::optional<TenantId> tenantId) {
            auto ret = reply->getBodyBuilder().asTempObj();
            CursorGetMoreReply::parse(
                IDLParserContext{"CursorGetMoreReply", false /* apiStrict */, tenantId},
                ret.removeField("ok"));
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
} getMoreCmd;

}  // namespace
}  // namespace mongo
