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

#include "mongo/db/service_entry_point_common.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/impersonation_session.h"
#include "mongo/db/client.h"
#include "mongo/db/command_can_run_here.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/handle_request_response.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/query/find.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/run_op_kill_cursors.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_factory.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/snapshot_window_util.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(rsStopGetMore);
MONGO_FAIL_POINT_DEFINE(respondWithNotPrimaryInCommandDispatch);
MONGO_FAIL_POINT_DEFINE(skipCheckingForNotPrimaryInCommandDispatch);
MONGO_FAIL_POINT_DEFINE(sleepMillisAfterCommandExecutionBegins);
MONGO_FAIL_POINT_DEFINE(waitAfterCommandFinishesExecution);
MONGO_FAIL_POINT_DEFINE(waitAfterNewStatementBlocksBehindPrepare);
MONGO_FAIL_POINT_DEFINE(hangBeforeSessionCheckOut);
MONGO_FAIL_POINT_DEFINE(hangBeforeSettingTxnInterruptFlag);
MONGO_FAIL_POINT_DEFINE(hangAfterCheckingWritabilityForMultiDocumentTransactions);

// Tracks the number of times a legacy unacknowledged write failed due to
// not primary error resulted in network disconnection.
Counter64 notPrimaryLegacyUnackWrites;
ServerStatusMetricField<Counter64> displayNotPrimaryLegacyUnackWrites(
    "repl.network.notPrimaryLegacyUnacknowledgedWrites", &notPrimaryLegacyUnackWrites);

// Tracks the number of times an unacknowledged write failed due to not primary error
// resulted in network disconnection.
Counter64 notPrimaryUnackWrites;
ServerStatusMetricField<Counter64> displayNotPrimaryUnackWrites(
    "repl.network.notPrimaryUnacknowledgedWrites", &notPrimaryUnackWrites);

namespace {
using logger::LogComponent;

void generateLegacyQueryErrorResponse(const AssertionException& exception,
                                      const QueryMessage& queryMessage,
                                      CurOp* curop,
                                      Message* response) {
    curop->debug().errInfo = exception.toStatus();

    log(LogComponent::kQuery) << "assertion " << exception.toString() << " ns:" << queryMessage.ns
                              << " query:"
                              << (queryMessage.query.valid(BSONVersion::kLatest)
                                      ? redact(queryMessage.query)
                                      : "query object is corrupt");
    if (queryMessage.ntoskip || queryMessage.ntoreturn) {
        log(LogComponent::kQuery) << " ntoskip:" << queryMessage.ntoskip
                                  << " ntoreturn:" << queryMessage.ntoreturn;
    }

    BSONObjBuilder err;
    err.append("$err", exception.reason());
    err.append("code", exception.code());
    err.append("ok", 0.0);
    auto const extraInfo = exception.extraInfo();
    if (extraInfo) {
        extraInfo->serialize(&err);
    }
    BSONObj errObj = err.done();

    const bool isStaleConfig = exception.code() == ErrorCodes::StaleConfig;
    if (isStaleConfig) {
        log(LogComponent::kQuery) << "stale version detected during query over " << queryMessage.ns
                                  << " : " << errObj;
    }

    BufBuilder bb;
    bb.skip(sizeof(QueryResult::Value));
    bb.appendBuf((void*)errObj.objdata(), errObj.objsize());

    // TODO: call replyToQuery() from here instead of this!!! see dbmessage.h
    QueryResult::View msgdata = bb.buf();
    QueryResult::View qr = msgdata;
    qr.setResultFlags(ResultFlag_ErrSet);
    if (isStaleConfig) {
        qr.setResultFlags(qr.getResultFlags() | ResultFlag_ShardConfigStale);
    }
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);
    response->setData(bb.release());
}

void registerError(OperationContext* opCtx, const DBException& exception) {
    LastError::get(opCtx->getClient()).setLastError(exception.code(), exception.reason());
    CurOp::get(opCtx)->debug().errInfo = exception.toStatus();
}

void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const DBException& exception,
                           const BSONObj& replyMetadata,
                           BSONObj extraFields = {}) {
    registerError(opCtx, exception);

    // We could have thrown an exception after setting fields in the builder,
    // so we need to reset it to a clean state just to be sure.
    replyBuilder->reset();
    replyBuilder->setCommandReply(exception.toStatus(), extraFields);
    replyBuilder->getBodyBuilder().appendElements(replyMetadata);
}

/**
 * Given the specified command, returns an effective read concern which should be used or an error
 * if the read concern is not valid for the command.
 */
StatusWith<repl::ReadConcernArgs> _extractReadConcern(const CommandInvocation* invocation,
                                                      const BSONObj& cmdObj,
                                                      bool upconvertToSnapshot) {
    repl::ReadConcernArgs readConcernArgs;

    auto readConcernParseStatus = readConcernArgs.initialize(cmdObj);
    if (!readConcernParseStatus.isOK()) {
        return readConcernParseStatus;
    }

    if (upconvertToSnapshot) {
        auto upconvertToSnapshotStatus = readConcernArgs.upconvertReadConcernLevelToSnapshot();
        if (!upconvertToSnapshotStatus.isOK()) {
            return upconvertToSnapshotStatus;
        }
    }

    if (!invocation->supportsReadConcern(readConcernArgs.getLevel())) {
        // We must be in a transaction if the readConcern level was upconverted to snapshot and the
        // command must support readConcern level snapshot in order to be supported in transactions.
        if (upconvertToSnapshot) {
            return {
                ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Command is not supported as the first command in a transaction"};
        }
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Command does not support read concern "
                              << readConcernArgs.toString()};
    }


    // If this command invocation asked for 'majority' read concern, supports blocking majority
    // reads, and storage engine support for majority reads is disabled, then we set the majority
    // read mechanism appropriately i.e. we utilize "speculative" read behavior.
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern &&
        invocation->allowsSpeculativeMajorityReads() &&
        !serverGlobalParams.enableMajorityReadConcern) {
        readConcernArgs.setMajorityReadMechanism(
            repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative);
    }

    return readConcernArgs;
}

/**
 * For replica set members it returns the last known op time from opCtx. Otherwise will return
 * uninitialized cluster time.
 */
LogicalTime getClientOperationTime(OperationContext* opCtx) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (!isReplSet) {
        return LogicalTime();
    }

    return LogicalTime(
        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp().getTimestamp());
}

/**
 * Returns the proper operationTime for a command. To construct the operationTime for replica set
 * members, it uses the last optime in the oplog for writes, last committed optime for majority
 * reads, and the last applied optime for every other read. An uninitialized cluster time is
 * returned for non replica set members.
 *
 * The latest in-memory clusterTime is returned if the start operationTime is uninitialized.
 */
LogicalTime computeOperationTime(OperationContext* opCtx, LogicalTime startOperationTime) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    invariant(isReplSet);

    auto operationTime = getClientOperationTime(opCtx);
    invariant(operationTime >= startOperationTime);

    // If the last operationTime has not changed, consider this command a read, and, for replica set
    // members, construct the operationTime with the proper optime for its read concern level.
    if (operationTime == startOperationTime) {
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

        // Note: ReadConcernArgs::getLevel returns kLocal if none was set.
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
            operationTime = LogicalTime(replCoord->getLastCommittedOpTime().getTimestamp());
        } else {
            operationTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
        }
    }

    return operationTime;
}

/**
 * Computes the proper $clusterTime and operationTime values to include in the command response and
 * appends them to it. $clusterTime is added as metadata and operationTime as a command body field.
 *
 * The command body BSONObjBuilder is either the builder for the command body itself, or a builder
 * for extra fields to be added to the reply when generating an error response.
 */
void appendClusterAndOperationTime(OperationContext* opCtx,
                                   BSONObjBuilder* commandBodyFieldsBob,
                                   BSONObjBuilder* metadataBob,
                                   LogicalTime startTime) {
    auto replicationCoordinator = repl::ReplicationCoordinator::get(opCtx);
    if (replicationCoordinator->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet ||
        !replicationCoordinator->getMemberState().readable() ||
        !LogicalClock::get(opCtx)->isEnabled()) {
        return;
    }

    // Authorized clients always receive operationTime and dummy signed $clusterTime.
    if (LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
        auto operationTime = computeOperationTime(opCtx, startTime);
        auto signedTime = SignedLogicalTime(
            LogicalClock::get(opCtx)->getClusterTime(), TimeProofService::TimeProof(), 0);

        dassert(signedTime.getTime() >= operationTime);
        rpc::LogicalTimeMetadata(signedTime).writeToMetadata(metadataBob);
        operationTime.appendAsOperationTime(commandBodyFieldsBob);

        return;
    }

    // Servers without validators (e.g. a shard server not yet added to a cluster) do not return
    // logical times to unauthorized clients.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (!validator) {
        return;
    }

    auto operationTime = computeOperationTime(opCtx, startTime);
    auto signedTime = validator->trySignLogicalTime(LogicalClock::get(opCtx)->getClusterTime());

    // If there were no keys, do not return $clusterTime or operationTime to unauthorized clients.
    if (signedTime.getKeyId() == 0) {
        return;
    }

    dassert(signedTime.getTime() >= operationTime);
    rpc::LogicalTimeMetadata(signedTime).writeToMetadata(metadataBob);
    operationTime.appendAsOperationTime(commandBodyFieldsBob);
}

namespace {
void _abortUnpreparedOrStashPreparedTransaction(
    OperationContext* opCtx, TransactionParticipant::Participant* txnParticipant) {
    const bool isPrepared = txnParticipant->transactionIsPrepared();
    try {
        if (isPrepared)
            txnParticipant->stashTransactionResources(opCtx);
        else if (txnParticipant->transactionIsOpen())
            txnParticipant->abortTransaction(opCtx);
    } catch (...) {
        // It is illegal for this to throw so we catch and log this here for diagnosability.
        severe() << "Caught exception during transaction " << opCtx->getTxnNumber()
                 << (isPrepared ? " stash " : " abort ") << opCtx->getLogicalSessionId()->toBSON()
                 << ": " << exceptionToStatus();
        std::terminate();
    }
}
}  // namespace

void invokeWithSessionCheckedOut(OperationContext* opCtx,
                                 CommandInvocation* invocation,
                                 const OperationSessionInfoFromClient& sessionOptions,
                                 rpc::ReplyBuilderInterface* replyBuilder) {
    // This constructor will check out the session. It handles the appropriate state management
    // for both multi-statement transactions and retryable writes. Currently, only requests with
    // a transaction number will check out the session.
    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeSessionCheckOut);
    MongoDOperationContextSession sessionTxnState(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    if (!opCtx->getClient()->isInDirectClient()) {
        bool beganOrContinuedTxn{false};
        // This loop allows new transactions on a session to block behind a previous prepared
        // transaction on that session.
        while (!beganOrContinuedTxn) {
            try {
                txnParticipant.beginOrContinue(opCtx,
                                               *sessionOptions.getTxnNumber(),
                                               sessionOptions.getAutocommit(),
                                               sessionOptions.getStartTransaction());
                beganOrContinuedTxn = true;
            } catch (const ExceptionFor<ErrorCodes::PreparedTransactionInProgress>&) {
                auto prepareCompleted = txnParticipant.onExitPrepare();

                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &waitAfterNewStatementBlocksBehindPrepare,
                    opCtx,
                    "waitAfterNewStatementBlocksBehindPrepare");

                // Check the session back in and wait for ongoing prepared transaction to complete.
                MongoDOperationContextSession::checkIn(opCtx);
                prepareCompleted.wait(opCtx);
                // Wait for the prepared commit or abort oplog entry to be visible in the oplog.
                // This will prevent a new transaction from missing the transaction table update for
                // the previous prepared commit or abort due to an oplog hole.
                auto storageInterface = repl::StorageInterface::get(opCtx);
                storageInterface->waitForAllEarlierOplogWritesToBeVisible(opCtx);
                MongoDOperationContextSession::checkOut(opCtx);
            }
        }

        // Create coordinator if needed. If "startTransaction" is present, it must be true.
        if (sessionOptions.getStartTransaction()) {
            // If this shard has been selected as the coordinator, set up the coordinator state
            // to be ready to receive votes.
            if (sessionOptions.getCoordinator() == boost::optional<bool>(true)) {
                createTransactionCoordinator(opCtx, *sessionOptions.getTxnNumber());
            }
        } else if (txnParticipant.transactionIsOpen()) {
            const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "Only the first command in a transaction may specify a readConcern",
                    readConcernArgs.isEmpty());
        }

        // Release the transaction lock resources and abort storage transaction for unprepared
        // transactions on failure to unstash the transaction resources to opCtx. We don't want to
        // have this error guard for beginOrContinue as it can abort the transaction for any
        // accidental invalid statements in the transaction.
        auto abortOnError = makeGuard([&txnParticipant, opCtx] {
            if (txnParticipant.transactionIsInProgress()) {
                txnParticipant.abortTransaction(opCtx);
            }
        });

        txnParticipant.unstashTransactionResources(opCtx, invocation->definition()->getName());

        // Unstash success.
        abortOnError.dismiss();
    }

    auto guard = makeGuard([opCtx, &txnParticipant] {
        _abortUnpreparedOrStashPreparedTransaction(opCtx, &txnParticipant);
    });

    try {
        invocation->run(opCtx, replyBuilder);
    } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&) {
        // Exceptions are used to resolve views in a sharded cluster, so they should be handled
        // specially to avoid unnecessary aborts.

        // If "startTransaction" is present, it must be true.
        if (sessionOptions.getStartTransaction()) {
            // If the first command a shard receives in a transactions fails with this code, the
            // shard may not be included in the final participant list if the router's retry after
            // resolving the view does not re-target it, which is possible if the underlying
            // collection is sharded. The shard's transaction should be preemptively aborted to
            // avoid leaving it orphaned in this case, which is fine even if it is re-targeted
            // because the retry will include "startTransaction" again and "restart" a transaction
            // at the active txnNumber.
            throw;
        }

        // If this shard has completed an earlier statement for this transaction, it must already be
        // in the transaction's participant list, so it is guaranteed to learn its outcome.
        txnParticipant.stashTransactionResources(opCtx);
        guard.dismiss();
        throw;
    } catch (const ExceptionFor<ErrorCodes::WouldChangeOwningShard>&) {
        txnParticipant.stashTransactionResources(opCtx);
        txnParticipant.resetRetryableWriteState(opCtx);
        guard.dismiss();
        throw;
    }

    if (auto okField = replyBuilder->getBodyBuilder().asTempObj()["ok"]) {
        // If ok is present, use its truthiness.
        if (!okField.trueValue()) {
            return;
        }
    }

    // Stash or commit the transaction when the command succeeds.
    txnParticipant.stashTransactionResources(opCtx);
    guard.dismiss();

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        auto txnResponseMetadata = txnParticipant.getResponseMetadata();
        auto bodyBuilder = replyBuilder->getBodyBuilder();
        txnResponseMetadata.serialize(&bodyBuilder);
    }
}

bool runCommandImpl(OperationContext* opCtx,
                    CommandInvocation* invocation,
                    const OpMsgRequest& request,
                    rpc::ReplyBuilderInterface* replyBuilder,
                    LogicalTime startOperationTime,
                    const ServiceEntryPointCommon::Hooks& behaviors,
                    BSONObjBuilder* extraFieldsBuilder,
                    const OperationSessionInfoFromClient& sessionOptions) {
    const Command* command = invocation->definition();
    auto bytesToReserve = command->reserveBytesForReply();
// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif
    replyBuilder->reserveBytes(bytesToReserve);

    const bool shouldCheckOutSession =
        sessionOptions.getTxnNumber() && !shouldCommandSkipSessionCheckout(command->getName());

    // getMore operations inherit a WriteConcern from their originating cursor. For example, if the
    // originating command was an aggregate with a $out and batchSize: 0. Note that if the command
    // only performed reads then we will not need to wait at all.
    const bool shouldWaitForWriteConcern =
        invocation->supportsWriteConcern() || command->getLogicalOp() == LogicalOp::opGetMore;

    if (shouldWaitForWriteConcern) {
        auto lastOpBeforeRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        // Change the write concern while running the command.
        const auto oldWC = opCtx->getWriteConcern();
        ON_BLOCK_EXIT([&] { opCtx->setWriteConcern(oldWC); });

        boost::optional<WriteConcernOptions> extractedWriteConcern;
        if (command->getLogicalOp() == LogicalOp::opGetMore) {
            // WriteConcern will be set up during command processing, it must not be specified on
            // the command body.
            behaviors.uassertCommandDoesNotSpecifyWriteConcern(request.body);
        } else {
            extractedWriteConcern.emplace(
                uassertStatusOK(extractWriteConcern(opCtx, request.body)));
            if (sessionOptions.getAutocommit()) {
                validateWriteConcernForTransaction(*extractedWriteConcern,
                                                   invocation->definition()->getName());
            }
            opCtx->setWriteConcern(*extractedWriteConcern);
        }

        auto waitForWriteConcern = [&](auto&& bb) {
            MONGO_FAIL_POINT_BLOCK_IF(failCommand, data, [&](const BSONObj& data) {
                return CommandHelpers::shouldActivateFailCommandFailPoint(
                           data, invocation, opCtx->getClient()) &&
                    data.hasField("writeConcernError");
            }) {
                bb.append(data.getData()["writeConcernError"]);
                return;  // Don't do normal waiting.
            }

            behaviors.waitForWriteConcern(opCtx, invocation, lastOpBeforeRun, bb);
        };

        try {
            if (shouldCheckOutSession) {
                invokeWithSessionCheckedOut(opCtx, invocation, sessionOptions, replyBuilder);
            } else {
                invocation->run(opCtx, replyBuilder);
            }
        } catch (const DBException& ex) {
            // Do no-op write before returning NoSuchTransaction if command has writeConcern.
            if (ex.toStatus().code() == ErrorCodes::NoSuchTransaction &&
                !opCtx->getWriteConcern().usedDefault) {
                TransactionParticipant::performNoopWrite(opCtx, "NoSuchTransaction error");
            }
            waitForWriteConcern(*extraFieldsBuilder);
            throw;
        }

        waitForWriteConcern(replyBuilder->getBodyBuilder());

        // With the exception of getMores inheriting the WriteConcern from the originating command,
        // nothing in run() should change the writeConcern.
        dassert(command->getLogicalOp() == LogicalOp::opGetMore
                    ? !extractedWriteConcern
                    : (extractedWriteConcern &&
                       SimpleBSONObjComparator::kInstance.evaluate(
                           opCtx->getWriteConcern().toBSON() == extractedWriteConcern->toBSON())));
    } else {
        behaviors.uassertCommandDoesNotSpecifyWriteConcern(request.body);
        if (shouldCheckOutSession) {
            invokeWithSessionCheckedOut(opCtx, invocation, sessionOptions, replyBuilder);
        } else {
            invocation->run(opCtx, replyBuilder);
        }
    }

    // This fail point blocks all commands which are running on the specified namespace, or which
    // are present in the given list of commands. If no namespace or command list are provided, then
    // the fail point will block all commands.
    MONGO_FAIL_POINT_BLOCK(waitAfterCommandFinishesExecution, options) {
        const BSONObj& data = options.getData();
        auto ns = data["ns"].valueStringDataSafe();
        auto commands =
            data.hasField("commands") ? data["commands"].Array() : std::vector<BSONElement>();

        // If 'ns' or 'commands' is not set, block for all the namespaces or commands respectively.
        if ((ns.empty() || invocation->ns().ns() == ns) &&
            (commands.empty() ||
             std::any_of(commands.begin(), commands.end(), [&request](auto& element) {
                 return element.valueStringDataSafe() == request.getCommandName();
             }))) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &waitAfterCommandFinishesExecution, opCtx, "waitAfterCommandFinishesExecution");
        }
    }

    behaviors.waitForLinearizableReadConcern(opCtx);

    // Wait for data to satisfy the read concern level, if necessary.
    behaviors.waitForSpeculativeMajorityReadConcern(opCtx);

    const bool ok = [&] {
        auto body = replyBuilder->getBodyBuilder();
        return CommandHelpers::extractOrAppendOk(body);
    }();
    behaviors.attachCurOpErrInfo(opCtx, replyBuilder->getBodyBuilder().asTempObj());

    if (!ok) {
        auto response = replyBuilder->getBodyBuilder().asTempObj();
        auto codeField = response["code"];
        const auto hasWriteConcern = response.hasField("writeConcernError");
        if (codeField.isNumber()) {
            auto code = ErrorCodes::Error(codeField.numberInt());
            // Append the error labels for transient transaction errors.
            auto errorLabels =
                getErrorLabels(sessionOptions, command->getName(), code, hasWriteConcern);
            replyBuilder->getBodyBuilder().appendElements(errorLabels);
        }
    }

    auto commandBodyBob = replyBuilder->getBodyBuilder();
    behaviors.appendReplyMetadata(opCtx, request, &commandBodyBob);
    appendClusterAndOperationTime(opCtx, &commandBodyBob, &commandBodyBob, startOperationTime);

    return ok;
}

/**
 * Executes a command after stripping metadata, performing authorization checks,
 * handling audit impersonation, and (potentially) setting maintenance mode. This method
 * also checks that the command is permissible to run on the node given its current
 * replication state. All the logic here is independent of any particular command; any
 * functionality relevant to a specific command should be confined to its run() method.
 */
void execCommandDatabase(OperationContext* opCtx,
                         Command* command,
                         const OpMsgRequest& request,
                         rpc::ReplyBuilderInterface* replyBuilder,
                         const ServiceEntryPointCommon::Hooks& behaviors) {
    CommandHelpers::uassertShouldAttemptParse(opCtx, command, request);
    BSONObjBuilder extraFieldsBuilder;
    auto startOperationTime = getClientOperationTime(opCtx);

    auto invocation = command->parse(opCtx, request);

    OperationSessionInfoFromClient sessionOptions;
    const auto isHello = command->getName() == "hello"_sd || command->getName() == "isMaster"_sd;

    try {
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setCommand_inlock(command);
        }

        if (isHello) {
            // Preload generic ClientMetadata ahead of our first hello request. After the first
            // request, metaElement should always be empty.
            auto metaElem = request.body[kMetadataDocumentName];
            ClientMetadata::setFromMetadata(opCtx->getClient(), metaElem);
        }

        MONGO_FAIL_POINT_BLOCK(sleepMillisAfterCommandExecutionBegins, arg) {
            const BSONObj& data = arg.getData();
            auto numMillis = data["millis"].numberInt();
            auto commands = data["commands"].Obj().getFieldNames<std::set<std::string>>();
            // Only sleep for one of the specified commands.
            if (commands.find(command->getName()) != commands.end()) {
                mongo::sleepmillis(numMillis);
            }
        }

        // TODO: move this back to runCommands when mongos supports OperationContext
        // see SERVER-18515 for details.
        rpc::readRequestMetadata(opCtx, request.body, command->requiresAuth());
        rpc::TrackingMetadata::get(opCtx).initWithOperName(command->getName());

        auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
        sessionOptions = initializeOperationSessionInfo(
            opCtx,
            request.body,
            command->requiresAuth(),
            command->attachLogicalSessionsToOpCtx(),
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet,
            opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking());

        CommandHelpers::evaluateFailCommandFailPoint(opCtx, invocation.get());

        const auto dbname = request.getDatabase().toString();
        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid database name: '" << dbname << "'",
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        validateSessionOptions(sessionOptions, command->getName(), dbname);

        BSONElement cmdOptionMaxTimeMSField;
        BSONElement allowImplicitCollectionCreationField;
        BSONElement helpField;

        StringMap<int> topLevelFields;
        for (auto&& element : request.body) {
            StringData fieldName = element.fieldNameStringData();
            if (fieldName == QueryRequest::cmdOptionMaxTimeMS) {
                cmdOptionMaxTimeMSField = element;
            } else if (fieldName == "allowImplicitCollectionCreation") {
                allowImplicitCollectionCreationField = element;
            } else if (fieldName == CommandHelpers::kHelpFieldName) {
                helpField = element;
            } else if (fieldName == QueryRequest::queryOptionMaxTimeMS) {
                uasserted(ErrorCodes::InvalidOptions,
                          "no such command option $maxTimeMs; use maxTimeMS instead");
            }

            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Parsed command object contains duplicate top level key: "
                                  << fieldName,
                    topLevelFields[fieldName]++ == 0);
        }

        if (CommandHelpers::isHelpRequest(helpField)) {
            CurOp::get(opCtx)->ensureStarted();
            // We disable last-error for help requests due to SERVER-11492, because config servers
            // use help requests to determine which commands are database writes, and so must be
            // forwarded to all config servers.
            LastError::get(opCtx->getClient()).disable();
            Command::generateHelpResponse(opCtx, replyBuilder, *command);
            return;
        }

        ImpersonationSessionGuard guard(opCtx);
        invocation->checkAuthorization(opCtx, request);

        const bool iAmPrimary = replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname);

        if (!opCtx->getClient()->isInDirectClient() &&
            !MONGO_FAIL_POINT(skipCheckingForNotPrimaryInCommandDispatch)) {
            const bool inMultiDocumentTransaction = (sessionOptions.getAutocommit() == false);

            // Kill this operation on step down even if it hasn't taken write locks yet, because it
            // could conflict with transactions from a new primary.
            if (inMultiDocumentTransaction) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeSettingTxnInterruptFlag, opCtx, "hangBeforeSettingTxnInterruptFlag");
                opCtx->setAlwaysInterruptAtStepDownOrUp();
            }

            auto allowed = command->secondaryAllowed(opCtx->getServiceContext());
            bool alwaysAllowed = allowed == Command::AllowedOnSecondary::kAlways;
            bool couldHaveOptedIn =
                allowed == Command::AllowedOnSecondary::kOptIn && !inMultiDocumentTransaction;
            bool optedIn =
                couldHaveOptedIn && ReadPreferenceSetting::get(opCtx).canRunOnSecondary();
            bool canRunHere = commandCanRunHere(opCtx, dbname, command, inMultiDocumentTransaction);
            if (!canRunHere && couldHaveOptedIn) {
                const auto msg = opCtx->getClient()->supportsHello()
                    ? "not primary and secondaryOk=false"_sd
                    : "not master and slaveOk=false"_sd;
                uasserted(ErrorCodes::NotPrimaryNoSecondaryOk, msg);
            }

            if (MONGO_FAIL_POINT(respondWithNotPrimaryInCommandDispatch)) {
                uassert(ErrorCodes::NotWritablePrimary, "not primary", canRunHere);
            } else {
                const auto msg =
                    opCtx->getClient()->supportsHello() ? "not primary"_sd : "not master"_sd;
                uassert(ErrorCodes::NotWritablePrimary, msg, canRunHere);
            }

            if (!command->maintenanceOk() &&
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                !replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname) &&
                !replCoord->getMemberState().secondary()) {

                uassert(ErrorCodes::NotPrimaryOrSecondary,
                        "node is recovering",
                        !replCoord->getMemberState().recovering());
                uassert(ErrorCodes::NotPrimaryOrSecondary,
                        "node is not in primary or recovering state",
                        replCoord->getMemberState().primary());
                // Check ticket SERVER-21432, slaveOk commands are allowed in drain mode
                uassert(ErrorCodes::NotPrimaryOrSecondary,
                        "node is in drain mode",
                        optedIn || alwaysAllowed);
            }

            // We acquire the RSTL which helps us here in two ways:
            // 1) It forces us to wait out any outstanding stepdown attempts.
            // 2) It guarantees that future RSTL holders will see the
            // 'setAlwaysInterruptAtStepDownOrUp' flag we set above.
            if (inMultiDocumentTransaction) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangAfterCheckingWritabilityForMultiDocumentTransactions,
                    opCtx,
                    "hangAfterCheckingWritabilityForMultiDocumentTransactions");
                repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
                uassert(ErrorCodes::NotWritablePrimary,
                        "Cannot start a transaction in a non-primary state",
                        replCoord->canAcceptWritesForDatabase(opCtx, dbname));
            }
        }

        if (command->adminOnly()) {
            LOG(2) << "command: " << request.getCommandName();
        }

        if (command->shouldAffectCommandCounter()) {
            OpCounters* opCounters = &globalOpCounters;
            opCounters->gotCommand();
        }

        // Parse the 'maxTimeMS' command option, and use it to set a deadline for the operation on
        // the OperationContext. The 'maxTimeMS' option unfortunately has a different meaning for a
        // getMore command, where it is used to communicate the maximum time to wait for new inserts
        // on tailable cursors, not as a deadline for the operation.
        // TODO SERVER-34277 Remove the special handling for maxTimeMS for getMores. This will
        // require introducing a new 'max await time' parameter for getMore, and eventually banning
        // maxTimeMS altogether on a getMore command.
        const int maxTimeMS =
            uassertStatusOK(QueryRequest::parseMaxTimeMS(cmdOptionMaxTimeMSField));
        if (maxTimeMS > 0 && command->getLogicalOp() != LogicalOp::opGetMore) {
            uassert(40119,
                    "Illegal attempt to set operation deadline within DBDirectClient",
                    !opCtx->getClient()->isInDirectClient());
            opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS}, ErrorCodes::MaxTimeMSExpired);
        }

        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

        // If the parent operation runs in a transaction, we don't override the read concern.
        auto skipReadConcern =
            opCtx->getClient()->isInDirectClient() && opCtx->inMultiDocumentTransaction();
        if (!skipReadConcern) {
            // If "startTransaction" is present, it must be true due to the parsing above.
            const bool upconvertToSnapshot(sessionOptions.getStartTransaction());
            auto newReadConcernArgs = uassertStatusOK(
                _extractReadConcern(invocation.get(), request.body, upconvertToSnapshot));
            {
                // We must obtain the client lock to set the ReadConcernArgs on the operation
                // context as it may be concurrently read by CurrentOp.
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                readConcernArgs = std::move(newReadConcernArgs);
            }
        }

        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            uassert(ErrorCodes::InvalidOptions,
                    "readConcern level snapshot is only valid for the first transaction operation",
                    opCtx->getClient()->isInDirectClient() || sessionOptions.getStartTransaction());
            uassert(ErrorCodes::InvalidOptions,
                    "readConcern level snapshot requires a session ID",
                    opCtx->getLogicalSessionId());
            uassert(ErrorCodes::InvalidOptions,
                    "readConcern level snapshot requires a txnNumber",
                    opCtx->getTxnNumber());

            opCtx->lockState()->setSharedLocksShouldTwoPhaseLock(true);
            opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);
        }

        auto& oss = OperationShardingState::get(opCtx);

        if (!opCtx->getClient()->isInDirectClient() &&
            readConcernArgs.getLevel() != repl::ReadConcernLevel::kAvailableReadConcern &&
            (iAmPrimary ||
             (readConcernArgs.hasLevel() || readConcernArgs.getArgsAfterClusterTime()))) {
            oss.initializeClientRoutingVersions(invocation->ns(), request.body);

            auto const shardingState = ShardingState::get(opCtx);
            if (oss.hasShardVersion() || oss.hasDbVersion()) {
                uassertStatusOK(shardingState->canAcceptShardedCommands());
            }

            behaviors.advanceConfigOpTimeFromRequestMetadata(opCtx);
        }

        oss.setAllowImplicitCollectionCreation(allowImplicitCollectionCreationField);
        auto scoped = behaviors.scopedOperationCompletionShardingActions(opCtx);

        // This may trigger the maxTimeAlwaysTimeOut failpoint.
        auto status = opCtx->checkForInterruptNoAssert();

        // We still proceed if the primary stepped down, but accept other kinds of interruptions.
        // We defer to individual commands to allow themselves to be interruptible by stepdowns,
        // since commands like 'voteRequest' should conversely continue executing.
        if (status != ErrorCodes::PrimarySteppedDown &&
            status != ErrorCodes::InterruptedDueToReplStateChange) {
            uassertStatusOK(status);
        }


        CurOp::get(opCtx)->ensureStarted();

        command->incrementCommandsExecuted();

        if (logger::globalLogDomain()->shouldLog(logger::LogComponent::kTracking,
                                                 logger::LogSeverity::Debug(1)) &&
            rpc::TrackingMetadata::get(opCtx).getParentOperId()) {
            MONGO_LOG_COMPONENT(1, logger::LogComponent::kTracking)
                << rpc::TrackingMetadata::get(opCtx).toString();
            rpc::TrackingMetadata::get(opCtx).setIsLogged(true);
        }

        behaviors.waitForReadConcern(opCtx, invocation.get(), request);

        try {
            if (!runCommandImpl(opCtx,
                                invocation.get(),
                                request,
                                replyBuilder,
                                startOperationTime,
                                behaviors,
                                &extraFieldsBuilder,
                                sessionOptions)) {
                command->incrementCommandsFailed();
            }
        } catch (const DBException& e) {
            command->incrementCommandsFailed();
            if (e.code() == ErrorCodes::Unauthorized) {
                CommandHelpers::auditLogAuthEvent(opCtx, invocation.get(), request, e.code());
            }
            throw;
        }
    } catch (const DBException& e) {
        if (e.code() == ErrorCodes::SnapshotTooOld) {
            // SnapshotTooOld errors should never be thrown unless we are using a storage engine
            // that supports snapshot read concern.
            auto engine = opCtx->getServiceContext()->getStorageEngine();
            invariant(engine && engine->supportsReadConcernSnapshot());

            // SnapshotTooOld errors indicate that PIT ops are failing to find an available snapshot
            // at their specified atClusterTime. Therefore, we'll try to increase the snapshot
            // history window that the storage engine maintains in order to increase the likelihood
            // of successful future PIT atClusterTime requests.
            SnapshotWindowUtil::incrementSnapshotTooOldErrorCount();
            SnapshotWindowUtil::increaseTargetSnapshotWindowSize(opCtx);
        } else {
            behaviors.handleException(e, opCtx);
        }

        // Append the error labels for transient transaction errors.
        auto response = extraFieldsBuilder.asTempObj();
        auto hasWriteConcern = response.hasField("writeConcernError");
        auto errorLabels =
            getErrorLabels(sessionOptions, command->getName(), e.code(), hasWriteConcern);
        extraFieldsBuilder.appendElements(errorLabels);

        BSONObjBuilder metadataBob;
        behaviors.appendReplyMetadata(opCtx, request, &metadataBob);

        // The read concern may not have yet been placed on the operation context, so attempt to
        // parse it here, so if it is valid it can be used to compute the proper operationTime.
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        if (readConcernArgs.isEmpty()) {
            auto readConcernArgsStatus = _extractReadConcern(invocation.get(), request.body, false);
            if (readConcernArgsStatus.isOK()) {
                // We must obtain the client lock to set the ReadConcernArgs on the operation
                // context as it may be concurrently read by CurrentOp.
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                readConcernArgs = readConcernArgsStatus.getValue();
            }
        }
        appendClusterAndOperationTime(opCtx, &extraFieldsBuilder, &metadataBob, startOperationTime);

        LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
               << "on database '" << request.getDatabase() << "' "
               << "with arguments '"
               << redact(ServiceEntryPointCommon::getRedactedCopyForLogging(command, request.body))
               << "': " << redact(e.toString());

        generateErrorResponse(opCtx, replyBuilder, e, metadataBob.obj(), extraFieldsBuilder.obj());
    }
}

/**
 * Fills out CurOp / OpDebug with basic command info.
 */
void curOpCommandSetup(OperationContext* opCtx, const OpMsgRequest& request) {
    auto curop = CurOp::get(opCtx);
    curop->debug().iscommand = true;

    // We construct a legacy $cmd namespace so we can fill in curOp using
    // the existing logic that existed for OP_QUERY commands
    NamespaceString nss(request.getDatabase(), "$cmd");

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    curop->setOpDescription_inlock(request.body);
    curop->markCommand_inlock();
    curop->setNS_inlock(nss.ns());
}

DbResponse receivedCommands(OperationContext* opCtx,
                            const Message& message,
                            const ServiceEntryPointCommon::Hooks& behaviors) {
    auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(message));
    OpMsgRequest request;
    [&] {
        try {  // Parse.
            request = rpc::opMsgRequestFromAnyProtocol(message);
        } catch (const DBException& ex) {
            // If this error needs to fail the connection, propagate it out.
            if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
                throw;

            BSONObjBuilder metadataBob;
            behaviors.appendReplyMetadataOnError(opCtx, &metadataBob);

            BSONObjBuilder extraFieldsBuilder;
            appendClusterAndOperationTime(
                opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

            // Otherwise, reply with the parse error. This is useful for cases where parsing fails
            // due to user-supplied input, such as the document too deep error. Since we failed
            // during parsing, we can't log anything about the command.
            LOG(1) << "assertion while parsing command: " << ex.toString();

            generateErrorResponse(
                opCtx, replyBuilder.get(), ex, metadataBob.obj(), extraFieldsBuilder.obj());

            return;  // From lambda. Don't try executing if parsing failed.
        }

        try {  // Execute.
            curOpCommandSetup(opCtx, request);

            Command* c = nullptr;
            // In the absence of a Command object, no redaction is possible. Therefore
            // to avoid displaying potentially sensitive information in the logs,
            // we restrict the log message to the name of the unrecognized command.
            // However, the complete command object will still be echoed to the client.
            if (!(c = CommandHelpers::findCommand(request.getCommandName()))) {
                globalCommandRegistry()->incrementUnknownCommands();
                std::string msg = str::stream()
                    << "no such command: '" << request.getCommandName() << "'";
                LOG(2) << msg;
                uasserted(ErrorCodes::CommandNotFound, str::stream() << msg);
            }

            LOG(2) << "run command " << request.getDatabase() << ".$cmd" << ' '
                   << redact(ServiceEntryPointCommon::getRedactedCopyForLogging(c, request.body));

            {
                // Try to set this as early as possible, as soon as we have figured out the command.
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setLogicalOp_inlock(c->getLogicalOp());
            }

            execCommandDatabase(opCtx, c, request, replyBuilder.get(), behaviors);
        } catch (const DBException& ex) {
            BSONObjBuilder metadataBob;
            behaviors.appendReplyMetadataOnError(opCtx, &metadataBob);

            BSONObjBuilder extraFieldsBuilder;
            appendClusterAndOperationTime(
                opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

            LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
                   << "on database '" << request.getDatabase() << "': " << ex.toString();

            generateErrorResponse(
                opCtx, replyBuilder.get(), ex, metadataBob.obj(), extraFieldsBuilder.obj());
        }
    }();

    if (OpMsg::isFlagSet(message, OpMsg::kMoreToCome)) {
        // Close the connection to get client to go through server selection again.
        if (LastError::get(opCtx->getClient()).hadNotPrimaryError()) {
            notPrimaryUnackWrites.increment();
            uasserted(ErrorCodes::NotWritablePrimary,
                      str::stream()
                          << "Not-primary error while processing '" << request.getCommandName()
                          << "' operation  on '" << request.getDatabase() << "' database via "
                          << "fire-and-forget command execution.");
        }
        return {};  // Don't reply.
    }

    DbResponse dbResponse;

    if (OpMsg::isFlagSet(message, OpMsg::kExhaustSupported)) {
        auto responseObj = replyBuilder->getBodyBuilder().asTempObj();
        auto cursorObj = responseObj.getObjectField("cursor");
        if (responseObj.getField("ok").trueValue() && !cursorObj.isEmpty()) {
            dbResponse.exhaustNS = cursorObj.getField("ns").String();
            dbResponse.exhaustCursorId = cursorObj.getField("id").numberLong();
        }
    }

    dbResponse.response = replyBuilder->done();
    CurOp::get(opCtx)->debug().responseLength = dbResponse.response.header().dataLen();

    return dbResponse;
}

DbResponse receivedQuery(OperationContext* opCtx,
                         const NamespaceString& nss,
                         Client& c,
                         const Message& m,
                         const ServiceEntryPointCommon::Hooks& behaviors) {
    invariant(!nss.isCommand());

    // The legacy opcodes should be counted twice: as part of the overall opcodes' counts and on
    // their own to highlight that they are being used.
    globalOpCounters.gotQuery();
    globalOpCounters.gotQueryDeprecated();

    ServerReadConcernMetrics::get(opCtx)->recordReadConcern(repl::ReadConcernArgs::get(opCtx));

    DbMessage d(m);
    QueryMessage q(d);

    CurOp& op = *CurOp::get(opCtx);
    DbResponse dbResponse;

    try {
        Client* client = opCtx->getClient();
        Status status = AuthorizationSession::get(client)->checkAuthForFind(nss, false);
        audit::logQueryAuthzCheck(client, nss, q.query, status.code());
        uassertStatusOK(status);

        dbResponse.exhaustNS = runQuery(opCtx, q, nss, dbResponse.response);
    } catch (const AssertionException& e) {
        behaviors.handleException(e, opCtx);

        dbResponse.response.reset();
        generateLegacyQueryErrorResponse(e, q, &op, &dbResponse.response);
    }

    op.debug().responseLength = dbResponse.response.header().dataLen();
    return dbResponse;
}

void receivedKillCursors(OperationContext* opCtx, const Message& m) {
    globalOpCounters.gotKillCursorsDeprecated();

    LastError::get(opCtx->getClient()).disable();
    DbMessage dbmessage(m);
    int n = dbmessage.pullInt();

    uassert(13659, "sent 0 cursors to kill", n != 0);
    massert(13658,
            str::stream() << "bad kill cursors size: " << m.dataSize(),
            m.dataSize() == 8 + (8 * n));
    uassert(13004, str::stream() << "sent negative cursors to kill: " << n, n >= 1);

    if (n > 2000) {
        (n < 30000 ? warning() : error()) << "receivedKillCursors, n=" << n;
        verify(n < 30000);
    }

    const char* cursorArray = dbmessage.getArray(n);

    int found = runOpKillCursors(opCtx, static_cast<size_t>(n), cursorArray);

    if (shouldLog(logger::LogSeverity::Debug(1)) || found != n) {
        LOG(found == n ? 1 : 0) << "killcursors: found " << found << " of " << n;
    }
}

void receivedInsert(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto insertOp = InsertOp::parseLegacy(m);
    invariant(insertOp.getNamespace() == nsString);

    globalOpCounters.gotInsertsDeprecated(insertOp.getDocuments().size());

    for (const auto& obj : insertOp.getDocuments()) {
        Status status =
            AuthorizationSession::get(opCtx->getClient())->checkAuthForInsert(opCtx, nsString);
        audit::logInsertAuthzCheck(opCtx->getClient(), nsString, obj, status.code());
        uassertStatusOK(status);
    }
    performInserts(opCtx, insertOp);
}

void receivedUpdate(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    globalOpCounters.gotUpdateDeprecated();
    auto updateOp = UpdateOp::parseLegacy(m);
    auto& singleUpdate = updateOp.getUpdates()[0];
    invariant(updateOp.getNamespace() == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForUpdate(opCtx,
                                             nsString,
                                             singleUpdate.getQ(),
                                             singleUpdate.getU(),
                                             singleUpdate.getUpsert());
    audit::logUpdateAuthzCheck(opCtx->getClient(),
                               nsString,
                               singleUpdate.getQ(),
                               singleUpdate.getU(),
                               singleUpdate.getUpsert(),
                               singleUpdate.getMulti(),
                               status.code());
    uassertStatusOK(status);

    performUpdates(opCtx, updateOp);
}

void receivedDelete(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    globalOpCounters.gotDeleteDeprecated();
    auto deleteOp = DeleteOp::parseLegacy(m);
    auto& singleDelete = deleteOp.getDeletes()[0];
    invariant(deleteOp.getNamespace() == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForDelete(opCtx, nsString, singleDelete.getQ());
    audit::logDeleteAuthzCheck(opCtx->getClient(), nsString, singleDelete.getQ(), status.code());
    uassertStatusOK(status);

    performDeletes(opCtx, deleteOp);
}

DbResponse receivedGetMore(OperationContext* opCtx,
                           const Message& m,
                           CurOp& curop,
                           bool* shouldLogOpDebug) {
    // The legacy opcodes should be counted twice: as part of the overall opcodes' counts and on
    // their own to highlight that they are being used.
    globalOpCounters.gotGetMore();
    globalOpCounters.gotGetMoreDeprecated();
    DbMessage d(m);

    const char* ns = d.getns();
    int ntoreturn = d.pullInt();
    uassert(
        34419, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    long long cursorid = d.pullInt64();

    curop.debug().ntoreturn = ntoreturn;
    curop.debug().cursorid = cursorid;

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(ns);
    }

    bool exhaust = false;
    bool isCursorAuthorized = false;

    DbResponse dbresponse;
    try {
        const NamespaceString nsString(ns);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid ns [" << ns << "]",
                nsString.isValid());

        Status status = AuthorizationSession::get(opCtx->getClient())
                            ->checkAuthForGetMore(nsString, cursorid, false);
        audit::logGetMoreAuthzCheck(opCtx->getClient(), nsString, cursorid, status.code());
        uassertStatusOK(status);

        while (MONGO_FAIL_POINT(rsStopGetMore)) {
            sleepmillis(0);
        }

        dbresponse.response =
            getMore(opCtx, ns, ntoreturn, cursorid, &exhaust, &isCursorAuthorized);
    } catch (AssertionException& e) {
        if (isCursorAuthorized) {
            // Make sure that killCursorGlobal does not throw an exception if it is interrupted.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // If an error was thrown prior to auth checks, then the cursor should remain alive in
            // order to prevent an unauthorized user from resulting in the death of a cursor. In
            // other error cases, the cursor is dead and should be cleaned up.
            //
            // If killing the cursor fails, ignore the error and don't try again. The cursor should
            // be reaped by the client cursor timeout thread.
            CursorManager::get(opCtx)
                ->killCursor(opCtx, cursorid, false /* shouldAudit */)
                .ignore();
        }

        BSONObjBuilder err;
        err.append("$err", e.reason());
        err.append("code", e.code());
        BSONObj errObj = err.obj();

        curop.debug().errInfo = e.toStatus();

        dbresponse = replyToQuery(errObj, ResultFlag_ErrSet);
        curop.debug().responseLength = dbresponse.response.header().dataLen();
        curop.debug().nreturned = 1;
        *shouldLogOpDebug = true;
        return dbresponse;
    }

    curop.debug().responseLength = dbresponse.response.header().dataLen();
    auto queryResult = QueryResult::ConstView(dbresponse.response.buf());
    curop.debug().nreturned = queryResult.getNReturned();

    if (exhaust) {
        curop.debug().exhaust = true;
        dbresponse.exhaustNS = ns;
    }

    return dbresponse;
}

}  // namespace

BSONObj ServiceEntryPointCommon::getRedactedCopyForLogging(const Command* command,
                                                           const BSONObj& cmdObj) {
    mutablebson::Document cmdToLog(cmdObj, mutablebson::Document::kInPlaceDisabled);
    command->snipForLogging(&cmdToLog);
    BSONObjBuilder bob;
    cmdToLog.writeTo(&bob);
    return bob.obj();
}

DbResponse ServiceEntryPointCommon::handleRequest(OperationContext* opCtx,
                                                  const Message& m,
                                                  const Hooks& behaviors) {
    // before we lock...
    NetworkOp op = m.operation();
    bool isCommand = false;

    DbMessage dbmsg(m);

    Client& c = *opCtx->getClient();

    if (c.isInDirectClient()) {
        if (!opCtx->getLogicalSessionId() || !opCtx->getTxnNumber()) {
            invariant(!opCtx->inMultiDocumentTransaction() &&
                      !opCtx->lockState()->inAWriteUnitOfWork());
        }
    } else {
        LastError::get(c).startRequest();
        AuthorizationSession::get(c)->startRequest(opCtx);

        // We should not be holding any locks at this point
        invariant(!opCtx->lockState()->isLocked());
    }

    const char* ns = dbmsg.messageShouldHaveNs() ? dbmsg.getns() : NULL;
    const NamespaceString nsString = ns ? NamespaceString(ns) : NamespaceString();

    if (op == dbQuery) {
        if (nsString.isCommand()) {
            isCommand = true;
        }
    } else if (op == dbMsg) {
        isCommand = true;
    }

    CurOp& currentOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // Commands handling code will reset this if the operation is a command
        // which is logically a basic CRUD operation like query, insert, etc.
        currentOp.setNetworkOp_inlock(op);
        currentOp.setLogicalOp_inlock(networkOpToLogicalOp(op));
    }

    OpDebug& debug = currentOp.debug();

    boost::optional<long long> slowMsOverride;
    bool forceLog = false;

    DbResponse dbresponse;
    if (op == dbMsg || (op == dbQuery && isCommand)) {
        dbresponse = receivedCommands(opCtx, m, behaviors);
    } else if (op == dbQuery) {
        invariant(!isCommand);
        opCtx->markKillOnClientDisconnect();

        dbresponse = receivedQuery(opCtx, nsString, c, m, behaviors);
    } else if (op == dbGetMore) {
        dbresponse = receivedGetMore(opCtx, m, currentOp, &forceLog);
    } else {
        // The remaining operations do not return any response. They are fire-and-forget.
        try {
            if (op == dbKillCursors) {
                currentOp.ensureStarted();
                slowMsOverride = 10;
                receivedKillCursors(opCtx, m);
            } else if (op != dbInsert && op != dbUpdate && op != dbDelete) {
                log() << "    operation isn't supported: " << static_cast<int>(op);
                currentOp.done();
                forceLog = true;
            } else {
                if (!opCtx->getClient()->isInDirectClient()) {
                    uassert(18663,
                            str::stream() << "legacy writeOps not longer supported for "
                                          << "versioned connections, ns: " << nsString.ns()
                                          << ", op: " << networkOpToString(op),
                            !ShardedConnectionInfo::get(&c, false));
                }

                if (!nsString.isValid()) {
                    uassert(16257, str::stream() << "Invalid ns [" << ns << "]", false);
                } else if (op == dbInsert) {
                    receivedInsert(opCtx, nsString, m);
                } else if (op == dbUpdate) {
                    receivedUpdate(opCtx, nsString, m);
                } else if (op == dbDelete) {
                    receivedDelete(opCtx, nsString, m);
                } else {
                    MONGO_UNREACHABLE;
                }
            }
        } catch (const AssertionException& ue) {
            LastError::get(c).setLastError(ue.code(), ue.reason());
            LOG(3) << " Caught Assertion in " << networkOpToString(op) << ", continuing "
                   << redact(ue);
            debug.errInfo = ue.toStatus();
        }
        // A NotWritablePrimary error can be set either within
        // receivedInsert/receivedUpdate/receivedDelete or within the AssertionException handler
        // above.  Either way, we want to throw an exception here, which will cause the client to be
        // disconnected.
        if (LastError::get(opCtx->getClient()).hadNotPrimaryError()) {
            notPrimaryLegacyUnackWrites.increment();
            uasserted(ErrorCodes::NotWritablePrimary,
                      str::stream()
                          << "Not-master error while processing '" << networkOpToString(op)
                          << "' operation  on '" << nsString << "' namespace via legacy "
                          << "fire-and-forget command execution.");
        }
    }

    // Mark the op as complete, and log it if appropriate. Returns a boolean indicating whether
    // this op should be sampled for profiling.
    const bool shouldSample = currentOp.completeAndLogOperation(
        opCtx, MONGO_LOG_DEFAULT_COMPONENT, dbresponse.response.size(), slowMsOverride, forceLog);

    Top::get(opCtx->getServiceContext())
        .incrementGlobalLatencyStats(
            opCtx,
            durationCount<Microseconds>(currentOp.elapsedTimeExcludingPauses()),
            currentOp.getReadWriteType());

    if (currentOp.shouldDBProfile(shouldSample)) {
        // Performance profiling is on
        if (opCtx->lockState()->isReadLocked()) {
            LOG(1) << "note: not profiling because recursive read lock";
        } else if (c.isInDirectClient()) {
            LOG(1) << "note: not profiling because we are in DBDirectClient";
        } else if (behaviors.lockedForWriting()) {
            // TODO SERVER-26825: Fix race condition where fsyncLock is acquired post
            // lockedForWriting() call but prior to profile collection lock acquisition.
            LOG(1) << "note: not profiling because doing fsync+lock";
        } else if (storageGlobalParams.readOnly) {
            LOG(1) << "note: not profiling because server is read-only";
        } else {
            invariant(!opCtx->lockState()->inAWriteUnitOfWork());
            profile(opCtx, op);
        }
    }

    recordCurOpMetrics(opCtx);
    return dbresponse;
}

ServiceEntryPointCommon::Hooks::~Hooks() = default;

}  // namespace mongo
