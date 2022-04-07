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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#define LOG_FOR_TRANSACTION(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kTransaction)

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_participant.h"

#include <fmt/format.h>

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {
using namespace fmt::literals;
namespace {

// Failpoint which will pause an operation just after allocating a point-in-time storage engine
// transaction.
MONGO_FAIL_POINT_DEFINE(hangAfterPreallocateSnapshot);

MONGO_FAIL_POINT_DEFINE(hangAfterReservingPrepareTimestamp);

MONGO_FAIL_POINT_DEFINE(hangAfterSettingPrepareStartTime);

MONGO_FAIL_POINT_DEFINE(hangBeforeReleasingTransactionOplogHole);

MONGO_FAIL_POINT_DEFINE(skipCommitTxnCheckPrepareMajorityCommitted);

MONGO_FAIL_POINT_DEFINE(restoreLocksFail);

const auto getTransactionParticipant = Session::declareDecoration<TransactionParticipant>();

// The command names that are allowed in a prepared transaction.
const StringMap<int> preparedTxnCmdWhitelist = {
    {"abortTransaction", 1}, {"commitTransaction", 1}, {"prepareTransaction", 1}};

void fassertOnRepeatedExecution(const LogicalSessionId& lsid,
                                TxnNumber txnNumber,
                                StmtId stmtId,
                                const repl::OpTime& firstOpTime,
                                const repl::OpTime& secondOpTime) {
    severe() << "Statement id " << stmtId << " from transaction [ " << lsid.toBSON() << ":"
             << txnNumber << " ] was committed once with opTime " << firstOpTime
             << " and a second time with opTime " << secondOpTime
             << ". This indicates possible data corruption or server bug and the process will be "
                "terminated.";
    fassertFailed(40526);
}

struct ActiveTransactionHistory {
    boost::optional<SessionTxnRecord> lastTxnRecord;
    TransactionParticipant::CommittedStatementTimestampMap committedStatements;
    bool hasIncompleteHistory{false};
};

ActiveTransactionHistory fetchActiveTransactionHistory(OperationContext* opCtx,
                                                       const LogicalSessionId& lsid) {
    // Restore the current timestamp read source after fetching transaction history.
    ReadSourceScope readSourceScope(opCtx);

    ActiveTransactionHistory result;

    result.lastTxnRecord = [&]() -> boost::optional<SessionTxnRecord> {
        DBDirectClient client(opCtx);
        // Even though the request only performs a read, the OpCtx's "in multi document transaction"
        // field has been set, bumping the global lock acquisition to an IX. That upconvert would
        // require a flow control ticket to be obtained.
        FlowControl::Bypass flowControlBypass(opCtx);
        auto result =
            client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                           {BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON())});
        if (result.isEmpty()) {
            return boost::none;
        }

        return SessionTxnRecord::parse(IDLParserErrorContext("parse latest txn record for session"),
                                       result);
    }();

    if (!result.lastTxnRecord) {
        return result;
    }

    // State is a new field in FCV 4.2 that indicates if a transaction committed, so check it in FCV
    // 4.2 and upgrading to 4.2. Check when downgrading as well so sessions refreshed at the start
    // of downgrade enter the correct state.
    if ((serverGlobalParams.featureCompatibility.getVersion() >=
             ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo40 &&
         result.lastTxnRecord->getState())) {
        // When state is given, it must be a transaction, so we don't need to traverse the history.
        return result;
    }

    auto it = TransactionHistoryIterator(result.lastTxnRecord->getLastWriteOpTime());
    while (it.hasNext()) {
        try {
            const auto entry = it.next(opCtx);

            // Each entry should correspond to a retryable write or a FCV4.0 format transaction.
            // These oplog entries must have statementIds.
            invariant(entry.getStatementId());
            if (*entry.getStatementId() == kIncompleteHistoryStmtId) {
                // Only the dead end sentinel can have this id for oplog write history
                invariant(entry.getObject2());
                invariant(entry.getObject2()->woCompare(TransactionParticipant::kDeadEndSentinel) ==
                          0);
                result.hasIncompleteHistory = true;
                continue;
            }


            // State is a new field in FCV 4.2, so look for an applyOps oplog entry without a
            // prepare flag to mark a committed transaction in FCV 4.0 or downgrading to 4.0. Check
            // when upgrading as well so sessions refreshed at the beginning of upgrade enter the
            // correct state.
            if ((serverGlobalParams.featureCompatibility.getVersion() <=
                 ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo42) &&
                (entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps &&
                 !entry.shouldPrepare() && !entry.isPartialTransaction())) {
                result.lastTxnRecord->setState(DurableTxnStateEnum::kCommitted);
                return result;
            }

            const auto insertRes =
                result.committedStatements.emplace(*entry.getStatementId(), entry.getOpTime());
            if (!insertRes.second) {
                const auto& existingOpTime = insertRes.first->second;
                fassertOnRepeatedExecution(lsid,
                                           result.lastTxnRecord->getTxnNum(),
                                           *entry.getStatementId(),
                                           existingOpTime,
                                           entry.getOpTime());
            }
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
                result.hasIncompleteHistory = true;
                break;
            }

            throw;
        }
    }

    return result;
}

void updateSessionEntry(OperationContext* opCtx, const UpdateRequest& updateRequest) {
    // Current code only supports replacement update.
    dassert(UpdateDriver::isDocReplacement(updateRequest.getUpdateModification()));

    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);

    uassert(40527,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            autoColl.getCollection());

    WriteUnitOfWork wuow(opCtx);

    auto collection = autoColl.getCollection();
    auto idIndex = collection->getIndexCatalog()->findIdIndex(opCtx);

    uassert(40672,
            str::stream() << "Failed to fetch _id index for "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns(),
            idIndex);

    auto indexAccess = collection->getIndexCatalog()->getEntry(idIndex)->accessMethod();
    // Since we are looking up a key inside the _id index, create a key object consisting of only
    // the _id field.
    auto idToFetch = updateRequest.getQuery().firstElement();
    auto toUpdateIdDoc = idToFetch.wrap();
    dassert(idToFetch.fieldNameStringData() == "_id"_sd);
    auto recordId = indexAccess->findSingle(opCtx, toUpdateIdDoc);
    auto startingSnapshotId = opCtx->recoveryUnit()->getSnapshotId();
    const auto updateMod = updateRequest.getUpdateModification().getUpdateClassic();

    if (recordId.isNull()) {
        // Upsert case.
        auto status = collection->insertDocument(opCtx, InsertStatement(updateMod), nullptr, false);

        if (status == ErrorCodes::DuplicateKey) {
            throw WriteConflictException();
        }

        uassertStatusOK(status);
        wuow.commit();
        return;
    }

    auto originalRecordData = collection->getRecordStore()->dataFor(opCtx, recordId);
    auto originalDoc = originalRecordData.toBson();

    invariant(collection->getDefaultCollator() == nullptr);
    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, nullptr));

    auto matcher =
        fassert(40673, MatchExpressionParser::parse(updateRequest.getQuery(), std::move(expCtx)));
    if (!matcher->matchesBSON(originalDoc)) {
        // Document no longer match what we expect so throw WCE to make the caller re-examine.
        throw WriteConflictException();
    }

    CollectionUpdateArgs args;
    args.update = updateMod;
    args.criteria = toUpdateIdDoc;
    args.fromMigrate = false;

    collection->updateDocument(opCtx,
                               recordId,
                               Snapshotted<BSONObj>(startingSnapshotId, originalDoc),
                               updateMod,
                               false,  // indexesAffected = false because _id is the only index
                               nullptr,
                               &args);

    wuow.commit();
}

// Failpoint which allows different failure actions to happen after each write. Supports the
// parameters below, which can be combined with each other (unless explicitly disallowed):
//
// closeConnection (bool, default = true): Closes the connection on which the write was executed.
// failBeforeCommitExceptionCode (int, default = not specified): If set, the specified exception
//      code will be thrown, which will cause the write to not commit; if not specified, the write
//      will be allowed to commit.
MONGO_FAIL_POINT_DEFINE(onPrimaryTransactionalWrite);

}  // namespace

const BSONObj TransactionParticipant::kDeadEndSentinel(BSON("$incompleteOplogHistory" << 1));

TransactionParticipant::TransactionParticipant() = default;

TransactionParticipant::~TransactionParticipant() {
    // invariant(!_o.txnState.isInProgress());
}

TransactionParticipant::Observer::Observer(const ObservableSession& osession)
    : Observer(&getTransactionParticipant(osession.get())) {}

TransactionParticipant::Participant::Participant(OperationContext* opCtx)
    : Observer([opCtx]() -> TransactionParticipant* {
          if (auto session = OperationContextSession::get(opCtx)) {
              return &getTransactionParticipant(session);
          }
          return nullptr;
      }()) {}

TransactionParticipant::Participant::Participant(const SessionToKill& session)
    : Observer(&getTransactionParticipant(session.get())) {}

void TransactionParticipant::performNoopWrite(OperationContext* opCtx, StringData msg) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // The locker must not have a max lock timeout when this noop write is performed, since if it
    // threw LockTimeout, this would be treated as a TransientTransactionError, which would indicate
    // it's safe to retry the entire transaction. We cannot know it is safe to attach
    // TransientTransactionError until the noop write has been performed and the writeConcern has
    // been satisfied.
    invariant(!opCtx->lockState()->hasMaxLockTimeout());

    {
        Lock::DBLock dbLock(opCtx, "local", MODE_IX);
        Lock::CollectionLock collectionLock(opCtx, NamespaceString::kRsOplogNamespace, MODE_IX);

        uassert(ErrorCodes::NotWritablePrimary,
                "Not primary when performing noop write for {}"_format(msg),
                replCoord->canAcceptWritesForDatabase(opCtx, "admin"));

        writeConflictRetry(
            opCtx, "performNoopWrite", NamespaceString::kRsOplogNamespace.ns(), [&opCtx, &msg] {
                WriteUnitOfWork wuow(opCtx);
                opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                    opCtx, BSON("msg" << msg));
                wuow.commit();
            });
    }
}

StorageEngine::OldestActiveTransactionTimestampResult
TransactionParticipant::getOldestActiveTimestamp(Timestamp stableTimestamp) {
    // Read from config.transactions at the stable timestamp for the oldest active transaction
    // timestamp. Use a short timeout: another thread might have the global lock e.g. to shut down
    // the server, and it both blocks this thread from querying config.transactions and waits for
    // this thread to terminate.
    auto client = getGlobalServiceContext()->makeClient("OldestActiveTxnTimestamp");
    AlternativeClientRegion acr(client);

    try {
        auto opCtx = cc().makeOperationContext();
        auto nss = NamespaceString::kSessionTransactionsTableNamespace;
        auto deadline = Date_t::now() + Milliseconds(100);

        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx->lockState());
        Lock::DBLock dbLock(opCtx.get(), nss.db(), MODE_IS, deadline);
        Lock::CollectionLock collLock(opCtx.get(), nss, MODE_IS, deadline);

        auto databaseHolder = DatabaseHolder::get(opCtx.get());
        auto db = databaseHolder->getDb(opCtx.get(), nss.db());
        if (!db) {
            // There is no config database, so there cannot be any active transactions.
            return boost::none;
        }

        auto collection = db->getCollection(opCtx.get(), nss);
        if (!collection) {
            return boost::none;
        }

        if (!stableTimestamp.isNull()) {
            opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                          stableTimestamp);
        }

        // Scan. We guess that occasional scans are cheaper than the write overhead of an index.
        boost::optional<Timestamp> oldestTxnTimestamp;
        auto cursor = collection->getCursor(opCtx.get());
        while (auto record = cursor->next()) {
            auto doc = record.get().data.toBson();
            auto txnRecord = SessionTxnRecord::parse(
                IDLParserErrorContext("parse oldest active txn record"), doc);
            if (txnRecord.getState() != DurableTxnStateEnum::kPrepared &&
                txnRecord.getState() != DurableTxnStateEnum::kInProgress) {
                continue;
            }
            // A prepared transaction must have a start timestamp.
            // TODO(SERVER-40013): Handle entries with state "prepared" and no "startTimestamp".
            invariant(txnRecord.getStartOpTime());
            auto ts = txnRecord.getStartOpTime()->getTimestamp();
            if (!oldestTxnTimestamp || ts < oldestTxnTimestamp.value()) {
                oldestTxnTimestamp = ts;
            }
        }

        return oldestTxnTimestamp;
    } catch (const DBException&) {
        return exceptionToStatus();
    }
}

const LogicalSessionId& TransactionParticipant::Observer::_sessionId() const {
    const auto* owningSession = getTransactionParticipant.owner(_tp);
    return owningSession->getSessionId();
}

void TransactionParticipant::Participant::_beginOrContinueRetryableWrite(OperationContext* opCtx,
                                                                         TxnNumber txnNumber) {
    if (txnNumber > o().activeTxnNumber) {
        // New retryable write.
        _setNewTxnNumber(opCtx, txnNumber);
        p().autoCommit = boost::none;
    } else {
        // Retrying a retryable write.
        uassert(ErrorCodes::IncompleteTransactionHistory,
                "Cannot retry a retryable write that has been converted into a transaction",
                o().txnState.isInRetryableWriteMode());
        invariant(p().autoCommit == boost::none);
    }
}

void TransactionParticipant::Participant::_continueMultiDocumentTransaction(OperationContext* opCtx,
                                                                            TxnNumber txnNumber) {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream()
                << "Given transaction number " << txnNumber
                << " does not match any in-progress transactions. The active transaction number is "
                << o().activeTxnNumber,
            txnNumber == o().activeTxnNumber && !o().txnState.isInRetryableWriteMode());

    if (o().txnState.isInProgress() && !o().txnResourceStash) {
        // This indicates that the first command in the transaction failed but did not implicitly
        // abort the transaction. It is not safe to continue the transaction, in particular because
        // we have not saved the readConcern from the first statement of the transaction. Mark the
        // transaction as active here, since _abortTransactionOnSession() will assume we are
        // aborting an active transaction since there are no stashed resources.
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).transactionMetricsObserver.onUnstash(
                ServerTransactionsMetrics::get(opCtx->getServiceContext()),
                opCtx->getServiceContext()->getTickSource());
        }
        _abortTransactionOnSession(opCtx);

        uasserted(
            ErrorCodes::NoSuchTransaction,
            str::stream()
                << "Transaction " << txnNumber
                << " has been aborted because an earlier command in this transaction failed.");
    }
    return;
}

void TransactionParticipant::Participant::_beginMultiDocumentTransaction(OperationContext* opCtx,
                                                                         TxnNumber txnNumber) {
    // Aborts any in-progress txns.
    _setNewTxnNumber(opCtx, txnNumber);
    p().autoCommit = false;

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).txnState.transitionTo(TransactionState::kInProgress);

    // Start tracking various transactions metrics.
    //
    // We measure the start time in both microsecond and millisecond resolution. The TickSource
    // provides microsecond resolution to record the duration of the transaction. The start "wall
    // clock" time can be considered an approximation to the microsecond measurement.
    auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();
    auto tickSource = opCtx->getServiceContext()->getTickSource();

    o(lk).transactionExpireDate = now + Seconds(gTransactionLifetimeLimitSeconds.load());

    o(lk).transactionMetricsObserver.onStart(
        ServerTransactionsMetrics::get(opCtx->getServiceContext()),
        *p().autoCommit,
        tickSource,
        now,
        *o().transactionExpireDate);
    invariant(p().transactionOperations.empty());
}

void TransactionParticipant::Participant::beginOrContinue(OperationContext* opCtx,
                                                          TxnNumber txnNumber,
                                                          boost::optional<bool> autocommit,
                                                          boost::optional<bool> startTransaction) {
    // Make sure we are still a primary. We need to hold on to the RSTL through the end of this
    // method, as we otherwise risk stepping down in the interim and incorrectly updating the
    // transaction number, which can abort active transactions.
    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
    if (opCtx->writesAreReplicated()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassert(ErrorCodes::NotWritablePrimary,
                "Not primary so we cannot begin or continue a transaction",
                replCoord->canAcceptWritesForDatabase(opCtx, "admin"));
        // Disallow multi-statement transactions on shard servers that have
        // writeConcernMajorityJournalDefault=false unless enableTestCommands=true. But allow
        // retryable writes (autocommit == boost::none).
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Transactions are not allowed on shard servers when "
                "writeConcernMajorityJournalDefault=false",
                replCoord->getWriteConcernMajorityShouldJournal() ||
                    serverGlobalParams.clusterRole != ClusterRole::ShardServer || !autocommit ||
                    getTestCommandsEnabled());
    }

    if (txnNumber < o().activeTxnNumber) {
        const std::string currOperation =
            o().txnState.isInRetryableWriteMode() ? "retryable write" : "transaction";
        if (!autocommit) {
            uasserted(ErrorCodes::TransactionTooOld,
                      str::stream()
                          << "Retryable write with txnNumber " << txnNumber
                          << " is prohibited on session " << _sessionId() << " because a newer "
                          << currOperation << " with txnNumber " << o().activeTxnNumber
                          << " has already started on this session.");
        } else {
            uasserted(ErrorCodes::TransactionTooOld,
                      str::stream() << "Cannot start transaction " << txnNumber << " on session "
                                    << _sessionId() << " because a newer " << currOperation
                                    << " with txnNumber " << o().activeTxnNumber
                                    << " has already started on this session.");
        }
    }


    // Requests without an autocommit field are interpreted as retryable writes. They cannot specify
    // startTransaction, which is verified earlier when parsing the request.
    if (!autocommit) {
        invariant(!startTransaction);
        _beginOrContinueRetryableWrite(opCtx, txnNumber);
        return;
    }

    // Attempt to continue a multi-statement transaction. In this case, it is required that
    // autocommit be given as an argument on the request, and currently it can only be false, which
    // is verified earlier when parsing the request.
    invariant(*autocommit == false);
    invariant(opCtx->inMultiDocumentTransaction());

    if (!startTransaction) {
        _continueMultiDocumentTransaction(opCtx, txnNumber);
        return;
    }

    // Attempt to start a multi-statement transaction, which requires startTransaction be given as
    // an argument on the request. The 'startTransaction' argument currently can only be specified
    // as true, which is verified earlier, when parsing the request.
    invariant(*startTransaction);

    if (txnNumber == o().activeTxnNumber) {
        // Servers in a sharded cluster can start a new transaction at the active transaction number
        // to allow internal retries by routers on re-targeting errors, like
        // StaleShard/DatabaseVersion or SnapshotTooOld.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Only servers in a sharded cluster can start a new transaction at the active "
                "transaction number",
                serverGlobalParams.clusterRole != ClusterRole::None);

        // The active transaction number can only be reused if:
        // 1. The transaction participant is in retryable write mode and has not yet executed a
        // retryable write, or
        // 2. A transaction is aborted and has not been involved in a two phase commit.
        //
        // Assuming routers target primaries in increasing order of term and in the absence of
        // byzantine messages, this check should never fail.
        const auto restartableStates =
            TransactionState::kNone | TransactionState::kAbortedWithoutPrepare;
        uassert(50911,
                str::stream() << "Cannot start a transaction at given transaction number "
                              << txnNumber << " a transaction with the same number is in state "
                              << o().txnState,
                o().txnState.isInSet(restartableStates));
    }

    _beginMultiDocumentTransaction(opCtx, txnNumber);
}

void TransactionParticipant::Participant::beginOrContinueTransactionUnconditionally(
    OperationContext* opCtx, TxnNumber txnNumber) {
    invariant(opCtx->inMultiDocumentTransaction());

    // We don't check or fetch any on-disk state, so treat the transaction as 'valid' for the
    // purposes of this method and continue the transaction unconditionally
    p().isValid = true;

    if (o().activeTxnNumber != txnNumber) {
        _beginMultiDocumentTransaction(opCtx, txnNumber);
    } else {
        invariant(o().txnState.isInSet(TransactionState::kInProgress | TransactionState::kPrepared),
                  str::stream() << "Current state: " << o().txnState);
    }

    // Assume we need to write an abort if we abort this transaction.  This method is called only
    // on secondaries (in which case we never write anything) and when a new primary knows about
    // an in-progress transaction.  If a new primary knows about an in-progress transaction, it
    // needs an abort oplog entry to be written if aborted (because the new primary could not
    // have found out if there wasn't an oplog entry for the new primary).
    p().needToWriteAbortEntry = true;
}

SharedSemiFuture<void> TransactionParticipant::Participant::onExitPrepare() const {
    if (!o().txnState._exitPreparePromise) {
        // The participant is not in prepare, so just return a ready future.
        return Future<void>::makeReady();
    }

    // The participant is in prepare, so return a future that will be signaled when the participant
    // transitions out of prepare.
    return o().txnState._exitPreparePromise->getFuture();
}

void TransactionParticipant::Participant::_setReadSnapshot(OperationContext* opCtx,
                                                           repl::ReadConcernArgs readConcernArgs) {
    if (readConcernArgs.getArgsAtClusterTime()) {
        // Read concern code should have already set the timestamp on the recovery unit.
        const auto readTimestamp = readConcernArgs.getArgsAtClusterTime()->asTimestamp();
        const auto ruTs = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
        invariant(readTimestamp == ruTs,
                  "readTimestamp: {}, pointInTime: {}"_format(readTimestamp.toString(),
                                                              ruTs ? ruTs->toString() : "none"));

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).transactionMetricsObserver.onChooseReadTimestamp(readTimestamp);
    } else if (readConcernArgs.getOriginalLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        // For transactions with read concern level specified as 'snapshot', we will use
        // 'kAllDurableSnapshot' which ensures a snapshot with no 'holes'; that is, it is a state
        // of the system that could be reconstructed from the oplog.
        opCtx->recoveryUnit()->setTimestampReadSource(
            RecoveryUnit::ReadSource::kAllDurableSnapshot);

        const auto readTimestamp =
            repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).transactionMetricsObserver.onChooseReadTimestamp(readTimestamp);
    } else {
        // For transactions with read concern level specified as 'local' or 'majority', we will use
        // 'kNoTimestamp' which gives us the most recent snapshot.  This snapshot may reflect oplog
        // 'holes' from writes earlier than the last applied write which have not yet completed.
        // Using 'kNoTimestamp' ensures that transactions with mode 'local' are always able to read
        // writes from earlier transactions with mode 'local' on the same connection.
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        // Catalog conflicting timestamps must be set on primaries performing transactions.
        // However, secondaries performing oplog application must avoid setting
        // _catalogConflictTimestamp. Currently, only oplog application on secondaries can run
        // inside a transaction, thus `writesAreReplicated` is a suitable proxy to single out
        // transactions on primaries.
        if (opCtx->writesAreReplicated()) {
            // Since this snapshot may reflect oplog holes, record the most visible timestamp before
            // opening a storage transaction. This timestamp will be used later to detect any
            // changes in the catalog after a storage transaction is opened.
            opCtx->recoveryUnit()->setCatalogConflictingTimestamp(
                opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp());
        }
    }

    opCtx->recoveryUnit()->preallocateSnapshot();
}

TransactionParticipant::OplogSlotReserver::OplogSlotReserver(OperationContext* opCtx,
                                                             int numSlotsToReserve)
    : _opCtx(opCtx), _globalLock(opCtx, MODE_IX) {
    // Stash the transaction on the OperationContext on the stack. At the end of this function it
    // will be unstashed onto the OperationContext.
    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // Begin a new WUOW and reserve a slot in the oplog.
    WriteUnitOfWork wuow(opCtx);
    auto oplogInfo = repl::LocalOplogInfo::get(opCtx);
    _oplogSlots = oplogInfo->getNextOpTimes(opCtx, numSlotsToReserve);

    // Release the WUOW state since this WUOW is no longer in use.
    wuow.release();

    // We must lock the Client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    // Save the RecoveryUnit from the new transaction and replace it with an empty one.
    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    // End two-phase locking on locker manually since the WUOW has been released.
    _opCtx->lockState()->endWriteUnitOfWork();
}

TransactionParticipant::OplogSlotReserver::~OplogSlotReserver() {
    if (MONGO_FAIL_POINT(hangBeforeReleasingTransactionOplogHole)) {
        log()
            << "transaction - hangBeforeReleasingTransactionOplogHole fail point enabled. Blocking "
               "until fail point is disabled.";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeReleasingTransactionOplogHole);
    }

    // If the constructor did not complete, we do not attempt to abort the units of work.
    if (_recoveryUnit) {
        // We should be at WUOW nesting level 1, only the top level WUOW for the oplog reservation
        // side transaction.
        _recoveryUnit->abortUnitOfWork();
    }
}

TransactionParticipant::TxnResources::TxnResources(WithLock wl,
                                                   OperationContext* opCtx,
                                                   StashStyle stashStyle) noexcept {
    // We must hold the Client lock to change the Locker on the OperationContext. Hence the
    // WithLock.

    _ruState = opCtx->getWriteUnitOfWork()->release();
    opCtx->setWriteUnitOfWork(nullptr);

    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    // Inherit the locking setting from the original one.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(
        _locker->shouldConflictWithSecondaryBatchApplication());
    _locker->releaseTicket();
    _locker->unsetThreadId();
    if (opCtx->getLogicalSessionId()) {
        _locker->setDebugInfo("lsid: " + opCtx->getLogicalSessionId()->toBSON().toString());
    }

    // On secondaries, we yield the locks for transactions.
    if (stashStyle == StashStyle::kSecondary) {
        _lockSnapshot = std::make_unique<Locker::LockSnapshot>();
        // Transactions have at least a global IX lock. Invariant that we have something to release.
        invariant(_locker->releaseWriteUnitOfWorkAndUnlock(_lockSnapshot.get()));
    }

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = gMaxTransactionLockRequestTimeoutMillis.load();
    if (stashStyle == StashStyle::kPrimary && maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    // On secondaries, max lock timeout must not be set.
    invariant(!(stashStyle == StashStyle::kSecondary && opCtx->lockState()->hasMaxLockTimeout()));

    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    _readConcernArgs = repl::ReadConcernArgs::get(opCtx);
}

TransactionParticipant::TxnResources::~TxnResources() {
    if (!_released && _recoveryUnit) {
        // This should only be reached when aborting a transaction that isn't active, i.e.
        // when starting a new transaction before completing an old one.  So we should
        // be at WUOW nesting level 1 (only the top level WriteUnitOfWork).
        _recoveryUnit->abortUnitOfWork();
        // If locks are not yielded, release them.
        if (!_lockSnapshot) {
            _locker->endWriteUnitOfWork();
        }
        invariant(!_locker->inAWriteUnitOfWork());
    }
}

void TransactionParticipant::TxnResources::release(OperationContext* opCtx) {
    // Perform operations that can fail the release before marking the TxnResources as released.
    auto onError = makeGuard([&] {
        // Release any locks acquired as part of lock restoration.
        if (_lockSnapshot) {
            // WUOW should be released before unlocking.
            Locker::WUOWLockSnapshot dummyWUOWLockInfo;
            _locker->releaseWriteUnitOfWork(&dummyWUOWLockInfo);

            Locker::LockSnapshot dummyLockInfo;
            _locker->saveLockStateAndUnlock(&dummyLockInfo);
        }
        // Release the ticket if acquired.
        // restoreWriteUnitOfWorkAndLock() can reacquire the ticket as well.
        if (_locker->getClientState() != Locker::ClientState::kInactive) {
            _locker->releaseTicket();
        }
    });

    // Restore locks if they are yielded.
    if (_lockSnapshot) {
        invariant(!_locker->isLocked());
        // opCtx is passed in to enable the restoration to be interrupted.
        _locker->restoreWriteUnitOfWorkAndLock(opCtx, *_lockSnapshot);
    }
    _locker->reacquireTicket(opCtx);

    if (MONGO_FAIL_POINT(restoreLocksFail)) {
        uasserted(ErrorCodes::LockTimeout, str::stream() << "Lock restore failed due to failpoint");
    }

    invariant(!_released);
    _released = true;

    // Successfully reacquired the locks and tickets.
    onError.dismiss();
    _lockSnapshot.reset(nullptr);

    // It is necessary to lock the client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    // We intentionally do not capture the return value of swapLockState(), which is just an empty
    // locker. At the end of the operation, if the transaction is not complete, we will stash the
    // operation context's locker and replace it with a new empty locker.
    opCtx->swapLockState(std::move(_locker));
    opCtx->lockState()->updateThreadIdToCurrentThread();

    auto oldState = opCtx->setRecoveryUnit(std::move(_recoveryUnit),
                                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    invariant(oldState == WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
              str::stream() << "RecoveryUnit state was " << oldState);

    opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(opCtx, _ruState));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    readConcernArgs = _readConcernArgs;
}

void TransactionParticipant::TxnResources::setNoEvictionAfterRollback() {
    _recoveryUnit->setNoEvictionAfterRollback();
}

TransactionParticipant::SideTransactionBlock::SideTransactionBlock(OperationContext* opCtx)
    : _opCtx(opCtx) {
    // Do nothing if we are already in a SideTransactionBlock. We can tell we are already in a
    // SideTransactionBlock because there is no top level write unit of work.
    if (!_opCtx->getWriteUnitOfWork()) {
        return;
    }

    // Release WUOW.
    _ruState = opCtx->getWriteUnitOfWork()->release();
    opCtx->setWriteUnitOfWork(nullptr);

    // Remember the locking state of WUOW, opt out two-phase locking, but don't release locks.
    opCtx->lockState()->releaseWriteUnitOfWork(&_WUOWLockSnapshot);

    // Release recovery unit, saving the recovery unit off to the side, keeping open the storage
    // transaction.
    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
}

TransactionParticipant::SideTransactionBlock::~SideTransactionBlock() {
    if (!_recoveryUnit) {
        return;
    }

    // Restore locker's state about WUOW.
    _opCtx->lockState()->restoreWriteUnitOfWork(_WUOWLockSnapshot);

    // Restore recovery unit.
    auto oldState = _opCtx->setRecoveryUnit(std::move(_recoveryUnit),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    invariant(oldState == WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
              str::stream() << "RecoveryUnit state was " << oldState);

    // Restore WUOW.
    _opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(_opCtx, _ruState));
}

void TransactionParticipant::Participant::_stashActiveTransaction(OperationContext* opCtx) {
    if (p().inShutdown) {
        return;
    }

    invariant(o().activeTxnNumber == opCtx->getTxnNumber());

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    {
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        o(lk).transactionMetricsObserver.onStash(ServerTransactionsMetrics::get(opCtx), tickSource);
        o(lk).transactionMetricsObserver.onTransactionOperation(
            opCtx, CurOp::get(opCtx)->debug().additiveMetrics, o().txnState.isPrepared());
    }

    invariant(!o().txnResourceStash);
    auto stashStyle = opCtx->writesAreReplicated() ? TxnResources::StashStyle::kPrimary
                                                   : TxnResources::StashStyle::kSecondary;
    o(lk).txnResourceStash = TxnResources(lk, opCtx, stashStyle);
}


void TransactionParticipant::Participant::stashTransactionResources(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }
    invariant(opCtx->getTxnNumber());

    if (o().txnState.isOpen()) {
        _stashActiveTransaction(opCtx);
    }
}

void TransactionParticipant::Participant::resetRetryableWriteState(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }
    invariant(opCtx->getTxnNumber());
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    if (o().txnState.isNone() && p().autoCommit == boost::none) {
        _resetRetryableWriteState();
    }
}

void TransactionParticipant::Participant::_releaseTransactionResourcesToOpCtx(
    OperationContext* opCtx, MaxLockTimeout maxLockTimeout, AcquireTicket acquireTicket) {
    // Transaction resources already exist for this transaction.  Transfer them from the
    // stash to the operation context.
    //
    // Because TxnResources::release must acquire the Client lock midway through, and because we
    // must hold the Client clock to mutate txnResourceStash, we jump through some hoops here to
    // move the TxnResources in txnResourceStash into a local variable that can be manipulated
    // without holding the Client lock.
    auto tempTxnResourceStash = [&]() noexcept {
        using std::swap;
        boost::optional<TxnResources> trs;
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        swap(trs, o(lk).txnResourceStash);
        return trs;
    }
    ();

    auto releaseOnError = makeGuard([&] {
        // Restore the lock resources back to transaction participant.
        using std::swap;
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        swap(o(lk).txnResourceStash, tempTxnResourceStash);
    });

    invariant(tempTxnResourceStash);
    auto stashLocker = tempTxnResourceStash->locker();
    invariant(stashLocker);

    if (maxLockTimeout == MaxLockTimeout::kNotAllowed) {
        stashLocker->unsetMaxLockTimeout();
    } else {
        // If maxTransactionLockRequestTimeoutMillis is set, then we will ensure no
        // future lock request waits longer than maxTransactionLockRequestTimeoutMillis
        // to acquire a lock. This is to avoid deadlocks and minimize non-transaction
        // operation performance degradations.
        auto maxTransactionLockMillis = gMaxTransactionLockRequestTimeoutMillis.load();
        if (maxTransactionLockMillis >= 0) {
            stashLocker->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
        }
    }

    if (acquireTicket == AcquireTicket::kSkip) {
        stashLocker->skipAcquireTicket();
    }

    tempTxnResourceStash->release(opCtx);
    releaseOnError.dismiss();
}

void TransactionParticipant::Participant::unstashTransactionResources(OperationContext* opCtx,
                                                                      const std::string& cmdName) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(opCtx->getTxnNumber());

    // If this is not a multi-document transaction, there is nothing to unstash.
    if (o().txnState.isInRetryableWriteMode()) {
        invariant(!o().txnResourceStash);
        return;
    }

    _checkIsCommandValidWithTxnState(*opCtx->getTxnNumber(), cmdName);
    if (o().txnResourceStash) {
        MaxLockTimeout maxLockTimeout;
        // Default is we should acquire ticket.
        AcquireTicket acquireTicket{AcquireTicket::kNoSkip};

        if (opCtx->writesAreReplicated()) {
            // Primaries should respect the transaction lock timeout, since it can prevent
            // the transaction from making progress.
            maxLockTimeout = MaxLockTimeout::kAllowed;
            // commitTransaction and abortTransaction commands can skip ticketing mechanism as they
            // don't acquire any new storage resources (except writing to oplog) but they release
            // any claimed storage resources.
            // Prepared transactions should not acquire ticket. Else, it can deadlock with other
            // non-transactional operations that have exhausted the write tickets and are blocked on
            // them due to prepare or lock conflict.
            if (o().txnState.isPrepared() || cmdName == "commitTransaction" ||
                cmdName == "abortTransaction") {
                acquireTicket = AcquireTicket::kSkip;
            }
        } else {
            // Max lock timeout must not be set on secondaries, since secondary oplog application
            // cannot fail.
            maxLockTimeout = MaxLockTimeout::kNotAllowed;
        }

        _releaseTransactionResourcesToOpCtx(opCtx, maxLockTimeout, acquireTicket);
        stdx::lock_guard<Client> lg(*opCtx->getClient());
        o(lg).transactionMetricsObserver.onUnstash(ServerTransactionsMetrics::get(opCtx),
                                                   opCtx->getServiceContext()->getTickSource());
        return;
    }

    // If we have no transaction resources then we cannot be prepared. If we're not in progress,
    // we don't do anything else.
    invariant(!o().txnState.isPrepared());

    if (!o().txnState.isInProgress()) {
        // At this point we're either committed and this is a 'commitTransaction' command, or we
        // are in the process of committing.
        return;
    }

    // All locks of transactions must be acquired inside the global WUOW so that we can
    // yield and restore all locks on state transition. Otherwise, we'd have to remember
    // which locks are managed by WUOW.
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->lockState()->isRSTLLocked());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Stashed transaction resources do not exist for this in-progress multi-document
    // transaction. Set up the transaction resources on the opCtx.
    opCtx->setWriteUnitOfWork(std::make_unique<WriteUnitOfWork>(opCtx));

    // If maxTransactionLockRequestTimeoutMillis is set, then we will ensure no
    // future lock request waits longer than maxTransactionLockRequestTimeoutMillis
    // to acquire a lock. This is to avoid deadlocks and minimize non-transaction
    // operation performance degradations.
    auto maxTransactionLockMillis = gMaxTransactionLockRequestTimeoutMillis.load();
    if (opCtx->writesAreReplicated() && maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    // On secondaries, max lock timeout must not be set.
    invariant(opCtx->writesAreReplicated() || !opCtx->lockState()->hasMaxLockTimeout());

    // Storage engine transactions may be started in a lazy manner. By explicitly
    // starting here we ensure that a point-in-time snapshot is established during the
    // first operation of a transaction.
    //
    // Active transactions are protected by the locking subsystem, so we must always hold at least a
    // Global intent lock before starting a transaction.  We pessimistically acquire an intent
    // exclusive lock here because we might be doing writes in this transaction, and it is currently
    // not deadlock-safe to upgrade IS to IX.
    boost::optional<Lock::GlobalLock> globalLock;
    // If the global lock acquisition fails, we must release the write unit of work to avoid an
    // invariant during _cleanUpTxnResourceOnOpCtx.
    try {
        globalLock.emplace(opCtx, MODE_IX);
    } catch (const DBException&) {
        opCtx->setWriteUnitOfWork(nullptr);
        throw;
    }

    // This begins the storage transaction and so we do it after acquiring the global lock.
    _setReadSnapshot(opCtx, repl::ReadConcernArgs::get(opCtx));

    // The Client lock must not be held when executing this failpoint as it will block currentOp
    // execution.
    if (MONGO_FAIL_POINT(hangAfterPreallocateSnapshot)) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangAfterPreallocateSnapshot, opCtx, "hangAfterPreallocateSnapshot");
    }

    {
        stdx::lock_guard<Client> lg(*opCtx->getClient());
        o(lg).transactionMetricsObserver.onUnstash(ServerTransactionsMetrics::get(opCtx),
                                                   opCtx->getServiceContext()->getTickSource());
    }
}

void TransactionParticipant::Participant::refreshLocksForPreparedTransaction(
    OperationContext* opCtx, bool yieldLocks) {
    // The opCtx will be used to swap locks, so it cannot hold any lock.
    invariant(!opCtx->lockState()->isRSTLLocked());
    invariant(!opCtx->lockState()->isLocked());

    // The node must have txn resource.
    invariant(o().txnResourceStash);
    invariant(o().txnState.isPrepared());

    // Lock and Ticket reacquisition of a prepared transaction should not fail for
    // state transitions (step up/step down).
    _releaseTransactionResourcesToOpCtx(opCtx, MaxLockTimeout::kNotAllowed, AcquireTicket::kNoSkip);

    // Snapshot transactions don't conflict with PBWM lock on both primary and secondary.
    invariant(!opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());

    // Transfer the txn resource back from the operation context to the stash.
    auto stashStyle =
        yieldLocks ? TxnResources::StashStyle::kSecondary : TxnResources::StashStyle::kPrimary;
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).txnResourceStash = TxnResources(lk, opCtx, stashStyle);
}

Timestamp TransactionParticipant::Participant::prepareTransaction(
    OperationContext* opCtx, boost::optional<repl::OpTime> prepareOptime) {

    auto abortGuard = makeGuard([&] {
        // Prepare transaction on secondaries should always succeed.
        invariant(!prepareOptime);

        try {
            // This shouldn't cause deadlocks with other prepared txns, because the acquisition
            // of RSTL lock inside abortTransaction will be no-op since we already have it.
            // This abortGuard gets dismissed before we release the RSTL while transitioning to
            // prepared.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            abortTransaction(opCtx);
        } catch (...) {
            // It is illegal for aborting a prepared transaction to fail for any reason, so we crash
            // instead.
            severe() << "Caught exception during abort of prepared transaction "
                     << opCtx->getTxnNumber() << " on " << _sessionId().toBSON() << ": "
                     << exceptionToStatus();
            std::terminate();
        }
    });

    auto& completedTransactionOperations = retrieveCompletedTransactionOperations(opCtx);

    // Ensure that no transaction operations were done against temporary collections.
    // Transactions should not operate on temporary collections because they are for internal use
    // only and are deleted on both repl stepup and server startup.

    // Create a set of collection UUIDs through which to iterate, so that we do not recheck the same
    // collection multiple times: it is a costly check.
    stdx::unordered_set<UUID, UUID::Hash> transactionOperationUuids;
    for (const auto& transactionOp : completedTransactionOperations) {
        transactionOperationUuids.insert(transactionOp.getUuid().get());
    }
    for (const auto& uuid : transactionOperationUuids) {
        auto collection = CollectionCatalog::get(opCtx).lookupCollectionByUUID(uuid);
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "prepareTransaction failed because one of the transaction "
                                 "operations was done against a temporary collection '"
                              << collection->ns() << "'.",
                !collection->isTemporary(opCtx));
    }

    boost::optional<OplogSlotReserver> oplogSlotReserver;
    OplogSlot prepareOplogSlot;
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // This check is necessary in order to avoid a race where a session with an active (but not
        // prepared) transaction is killed, but it still ends up in the prepared state
        opCtx->checkForInterrupt();
        o(lk).txnState.transitionTo(TransactionState::kPrepared);
    }
    std::vector<OplogSlot> reservedSlots;
    if (prepareOptime) {
        // On secondary, we just prepare the transaction and discard the buffered ops.
        prepareOplogSlot = OplogSlot(*prepareOptime);
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).prepareOpTime = *prepareOptime;
        reservedSlots.push_back(prepareOplogSlot);
    } else {
        // On primary, we reserve an optime, prepare the transaction and write the oplog entry.
        //
        // Reserve an optime for the 'prepareTimestamp'. This will create a hole in the oplog and
        // cause 'snapshot' and 'afterClusterTime' readers to block until this transaction is done
        // being prepared. When the OplogSlotReserver goes out of scope and is destroyed, the
        // storage-transaction it uses to keep the hole open will abort and the slot (and
        // corresponding oplog hole) will vanish.
        // TODO(SERVER-41470): Remove the if-clause here.
        if (serverGlobalParams.featureCompatibility.getVersion() <
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
            oplogSlotReserver.emplace(opCtx);
        } else {
            // Even if the prepared transaction contained no statements, we always reserve at least
            // 1 oplog slot for the prepare oplog entry.
            const auto numSlotsToReserve = retrieveCompletedTransactionOperations(opCtx).size();
            oplogSlotReserver.emplace(opCtx, std::max(1, static_cast<int>(numSlotsToReserve)));
            invariant(oplogSlotReserver->getSlots().size() >= 1);
        }
        prepareOplogSlot = oplogSlotReserver->getLastSlot();
        reservedSlots = oplogSlotReserver->getSlots();
        invariant(o().prepareOpTime.isNull(),
                  str::stream() << "This transaction has already reserved a prepareOpTime at: "
                                << o().prepareOpTime.toString());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).prepareOpTime = prepareOplogSlot;
        }

        if (MONGO_FAIL_POINT(hangAfterReservingPrepareTimestamp)) {
            // This log output is used in js tests so please leave it.
            log() << "transaction - hangAfterReservingPrepareTimestamp fail point "
                     "enabled. Blocking until fail point is disabled. Prepare OpTime: "
                  << prepareOplogSlot;
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterReservingPrepareTimestamp);
        }
    }
    opCtx->recoveryUnit()->setPrepareTimestamp(prepareOplogSlot.getTimestamp());
    opCtx->getWriteUnitOfWork()->prepare();
    p().needToWriteAbortEntry = true;
    opCtx->getServiceContext()->getOpObserver()->onTransactionPrepare(
        opCtx, reservedSlots, completedTransactionOperations);

    abortGuard.dismiss();

    {
        const auto ticks = opCtx->getServiceContext()->getTickSource()->getTicks();
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).transactionMetricsObserver.onPrepare(ServerTransactionsMetrics::get(opCtx), ticks);

        // Ensure the lastWriteOpTime is set. This is needed so that we can correctly assign the
        // prevOpTime for commit and abort oplog entries if a failover happens after the prepare.
        // This value is updated in _registerCacheUpdateOnCommit, but only on primaries. We
        // update the lastWriteOpTime here so that it is also available to secondaries. We can
        // count on it to persist since we never invalidate prepared transactions.
        o(lk).lastWriteOpTime = prepareOplogSlot;
    }

    if (MONGO_FAIL_POINT(hangAfterSettingPrepareStartTime)) {
        log() << "transaction - hangAfterSettingPrepareStartTime fail point enabled. Blocking "
                 "until fail point is disabled.";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterSettingPrepareStartTime);
    }

    // We unlock the RSTL to allow prepared transactions to survive state transitions. This should
    // be the last thing we do since a state transition may happen immediately after releasing the
    // RSTL.
    const bool unlocked = opCtx->lockState()->unlockRSTLforPrepare();
    invariant(unlocked);

    return prepareOplogSlot.getTimestamp();
}

void TransactionParticipant::Participant::setPrepareOpTimeForRecovery(OperationContext* opCtx,
                                                                      repl::OpTime prepareOpTime) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).recoveryPrepareOpTime = prepareOpTime;
}

const repl::OpTime TransactionParticipant::Participant::getPrepareOpTimeForRecovery() const {
    return o().recoveryPrepareOpTime;
}

void TransactionParticipant::Participant::addTransactionOperation(
    OperationContext* opCtx, const repl::ReplOperation& operation) {

    // Ensure that we only ever add operations to an in progress transaction.
    invariant(o().txnState.isInProgress(), str::stream() << "Current state: " << o().txnState);

    invariant(p().autoCommit && !*p().autoCommit && o().activeTxnNumber != kUninitializedTxnNumber);
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    p().transactionOperations.push_back(operation);
    p().transactionOperationBytes += repl::OplogEntry::getDurableReplOperationSize(operation);

    auto transactionSizeLimitBytes = gTransactionSizeLimitBytes.load();
    uassert(ErrorCodes::TransactionTooLarge,
            str::stream() << "Total size of all transaction operations must be less than "
                          << "server parameter 'transactionSizeLimitBytes' = "
                          << transactionSizeLimitBytes,
            p().transactionOperationBytes <= static_cast<size_t>(transactionSizeLimitBytes));

    // Creating transactions larger than 16MB requires a new oplog format only available in FCV 4.2.
    const auto isFCV42 = serverGlobalParams.featureCompatibility.getVersion() ==
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42;
    // _transactionOperationBytes is based on the in-memory size of the operation.  With overhead,
    // we expect the BSON size of the operation to be larger, so it's possible to make a transaction
    // just a bit too large and have it fail only in the commit.  It's still useful to fail early
    // when possible (e.g. to avoid exhausting server memory).
    uassert(ErrorCodes::TransactionTooLarge,
            str::stream() << "Total size of all transaction operations must be less than "
                          << BSONObjMaxInternalSize
                          << " when using featureCompatibilityVersion < 4.2. Actual size is "
                          << p().transactionOperationBytes,
            isFCV42 || p().transactionOperationBytes <= BSONObjMaxInternalSize);
}

std::vector<repl::ReplOperation>&
TransactionParticipant::Participant::retrieveCompletedTransactionOperations(
    OperationContext* opCtx) {

    // Ensure that we only ever retrieve a transaction's completed operations when in progress
    // or prepared.
    invariant(o().txnState.isInSet(TransactionState::kInProgress | TransactionState::kPrepared),
              str::stream() << "Current state: " << o().txnState);

    return p().transactionOperations;
}

TxnResponseMetadata TransactionParticipant::Participant::getResponseMetadata() {
    // Currently the response metadata only contains a single field, which is whether or not the
    // transaction is read-only so far.
    return {o().txnState.isInSet(TransactionState::kInProgress) &&
            p().transactionOperations.empty()};
}

void TransactionParticipant::Participant::clearOperationsInMemory(OperationContext* opCtx) {
    // Ensure that we only ever end a prepared or in-progress transaction.
    invariant(o().txnState.isInSet(TransactionState::kPrepared | TransactionState::kInProgress),
              str::stream() << "Current state: " << o().txnState);
    invariant(p().autoCommit);
    p().transactionOperationBytes = 0;
    p().transactionOperations.clear();
}

void TransactionParticipant::Participant::commitUnpreparedTransaction(OperationContext* opCtx) {
    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction must provide commitTimestamp to prepared transaction.",
            !o().txnState.isPrepared());

    auto txnOps = retrieveCompletedTransactionOperations(opCtx);
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);

    opObserver->onUnpreparedTransactionCommit(opCtx, txnOps);

    // Read-only transactions with all read concerns must wait for any data they read to be majority
    // committed. For local read concern this is to match majority read concern. For both local and
    // majority read concerns we do an untimestamped read, so we have no read timestamp to wait on.
    // Instead, we write a noop which is guaranteed to have a greater OpTime than any writes we
    // read.
    //
    // TODO (SERVER-41165): Snapshot read concern should wait on the read timestamp instead.
    auto wc = opCtx->getWriteConcern();
    auto needsNoopWrite = txnOps.empty() && !opCtx->getWriteConcern().usedDefault;

    const size_t operationCount = p().transactionOperations.size();
    const size_t oplogOperationBytes = p().transactionOperationBytes;
    clearOperationsInMemory(opCtx);

    try {
        // Once committing we cannot throw an exception.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        _commitStorageTransaction(opCtx);
        _finishCommitTransaction(opCtx, operationCount, oplogOperationBytes);
    } catch (...) {
        // It is illegal for committing a transaction to fail for any reason, other than an
        // invalid command, so we crash instead.
        severe() << "Caught exception during commit of unprepared transaction "
                 << opCtx->getTxnNumber() << " on " << _sessionId().toBSON() << ": "
                 << exceptionToStatus();
        std::terminate();
    }

    if (needsNoopWrite) {
        performNoopWrite(
            opCtx, str::stream() << "read-only transaction with writeConcern " << wc.toBSON());
    }
}

void TransactionParticipant::Participant::commitPreparedTransaction(
    OperationContext* opCtx,
    Timestamp commitTimestamp,
    boost::optional<repl::OpTime> commitOplogEntryOpTime) {
    // A correctly functioning coordinator could hit this uassert. This could happen if this
    // participant shard failed over and the new primary majority committed prepare without this
    // node in its majority. The coordinator could legally send commitTransaction with a
    // commitTimestamp to this shard but target the old primary (this node) that has yet to prepare
    // the transaction. We uassert since this node cannot commit the transaction.
    if (!o().txnState.isPrepared()) {
        uasserted(ErrorCodes::InvalidOptions,
                  "commitTransaction cannot provide commitTimestamp to unprepared transaction.");
    }

    // Re-acquire the RSTL to prevent state transitions while committing the transaction. When the
    // transaction was prepared, we dropped the RSTL. We do not need to reacquire the PBWM because
    // if we're not the primary we will uassert anyways.
    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

    // Prepared transactions cannot hold the RSTL, or else they will deadlock with state
    // transitions. If we do not commit the transaction we must unlock the RSTL explicitly so two
    // phase locking doesn't hold onto it.
    auto unlockGuard = makeGuard([&] { invariant(opCtx->lockState()->unlockRSTLforPrepare()); });

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    if (opCtx->writesAreReplicated()) {
        uassert(ErrorCodes::NotWritablePrimary,
                "Not primary so we cannot commit a prepared transaction",
                replCoord->canAcceptWritesForDatabase(opCtx, "admin"));
    }

    uassert(
        ErrorCodes::InvalidOptions, "'commitTimestamp' cannot be null", !commitTimestamp.isNull());

    const auto prepareTimestamp = o().prepareOpTime.getTimestamp();

    uassert(ErrorCodes::InvalidOptions,
            "'commitTimestamp' must be greater than or equal to 'prepareTimestamp'",
            commitTimestamp >= prepareTimestamp);

    if (!commitOplogEntryOpTime) {
        // A correctly functioning coordinator could hit this uassert. This could happen if this
        // participant shard failed over and the new primary majority committed prepare but has yet
        // to communicate that to this node. The coordinator could legally send commitTransaction
        // with a commitTimestamp to this shard but target the old primary (this node) that does not
        // yet know prepare is majority committed. We uassert since the commit oplog entry would be
        // written in an old term and be guaranteed to roll back. This makes it easier to write
        // correct tests, consider fewer participant commit cases, and catch potential bugs since
        // hitting this uassert correctly is unlikely.
        uassert(ErrorCodes::InvalidOptions,
                "commitTransaction for a prepared transaction cannot be run before its prepare "
                "oplog entry has been majority committed",
                replCoord->getLastCommittedOpTime().getTimestamp() >= prepareTimestamp ||
                    MONGO_FAIL_POINT(skipCommitTxnCheckPrepareMajorityCommitted));
    }

    try {
        // We can no longer uassert without terminating.
        unlockGuard.dismiss();

        // Once entering "committing with prepare" we cannot throw an exception.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        opCtx->recoveryUnit()->setCommitTimestamp(commitTimestamp);

        // On secondary, we generate a fake empty oplog slot, since it's not used by opObserver.
        OplogSlot commitOplogSlot;
        boost::optional<OplogSlotReserver> oplogSlotReserver;

        // On primary, we reserve an oplog slot before committing the transaction so that no
        // writes that are causally related to the transaction commit enter the oplog at a
        // timestamp earlier than the commit oplog entry.
        if (opCtx->writesAreReplicated()) {
            invariant(!commitOplogEntryOpTime);
            oplogSlotReserver.emplace(opCtx);
            commitOplogSlot = oplogSlotReserver->getLastSlot();
            invariant(commitOplogSlot.getTimestamp() >= commitTimestamp,
                      str::stream() << "Commit oplog entry must be greater than or equal to commit "
                                       "timestamp due to causal consistency. commit timestamp: "
                                    << commitTimestamp.toBSON()
                                    << ", commit oplog entry optime: " << commitOplogSlot.toBSON());
        } else {
            // We always expect a non-null commitOplogEntryOpTime to be passed in on secondaries
            // in order to set the finishOpTime.
            invariant(commitOplogEntryOpTime);
        }

        // We must have a lastWriteOpTime set, as that will be used for the prevOpTime on the oplog
        // entry.
        invariant(!o().lastWriteOpTime.isNull());

        // If commitOplogEntryOpTime is a nullopt, then we grab the OpTime from the commitOplogSlot
        // which will only be set if we are primary. Otherwise, the commitOplogEntryOpTime must have
        // been passed in during secondary oplog application.
        auto commitOplogSlotOpTime = commitOplogEntryOpTime.value_or(commitOplogSlot);
        opCtx->recoveryUnit()->setDurableTimestamp(commitOplogSlotOpTime.getTimestamp());

        _commitStorageTransaction(opCtx);

        auto opObserver = opCtx->getServiceContext()->getOpObserver();
        invariant(opObserver);

        // Once the transaction is committed, the oplog entry must be written.
        opObserver->onPreparedTransactionCommit(
            opCtx, commitOplogSlot, commitTimestamp, retrieveCompletedTransactionOperations(opCtx));

        const size_t operationCount = p().transactionOperations.size();
        const size_t oplogOperationBytes = p().transactionOperationBytes;
        clearOperationsInMemory(opCtx);

        _finishCommitTransaction(opCtx, operationCount, oplogOperationBytes);
    } catch (...) {
        // It is illegal for committing a prepared transaction to fail for any reason, other than an
        // invalid command, so we crash instead.
        severe() << "Caught exception during commit of prepared transaction "
                 << opCtx->getTxnNumber() << " on " << _sessionId().toBSON() << ": "
                 << exceptionToStatus();
        std::terminate();
    }
}

void TransactionParticipant::Participant::_commitStorageTransaction(OperationContext* opCtx) try {
    invariant(opCtx->getWriteUnitOfWork());
    invariant(opCtx->lockState()->isRSTLLocked());
    opCtx->getWriteUnitOfWork()->commit();
    opCtx->setWriteUnitOfWork(nullptr);

    // We must clear the recovery unit and locker for the 'config.transactions' and oplog entry
    // writes.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
} catch (...) {
    // It is illegal for committing a storage-transaction to fail so we crash instead.
    severe() << "Caught exception during commit of storage-transaction " << opCtx->getTxnNumber()
             << " on " << _sessionId().toBSON() << ": " << exceptionToStatus();
    std::terminate();
}

void TransactionParticipant::Participant::_finishCommitTransaction(OperationContext* opCtx,
                                                                   size_t operationCount,
                                                                   size_t oplogOperationBytes) {
    {
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).txnState.transitionTo(TransactionState::kCommitted);

        o(lk).transactionMetricsObserver.onCommit(opCtx,
                                                  ServerTransactionsMetrics::get(opCtx),
                                                  tickSource,
                                                  &Top::get(getGlobalServiceContext()),
                                                  operationCount,
                                                  oplogOperationBytes);
        o(lk).transactionMetricsObserver.onTransactionOperation(
            opCtx, CurOp::get(opCtx)->debug().additiveMetrics, o().txnState.isPrepared());
    }
    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    _cleanUpTxnResourceOnOpCtx(opCtx, TerminationCause::kCommitted);
}

void TransactionParticipant::Participant::shutdown(OperationContext* opCtx) {
    stdx::lock_guard<Client> lock(*opCtx->getClient());

    p().inShutdown = true;
    o(lock).txnResourceStash = boost::none;
}

bool TransactionParticipant::Observer::expiredAsOf(Date_t when) const {
    return o().txnState.isInProgress() && o().transactionExpireDate &&
        o().transactionExpireDate < when;
}

void TransactionParticipant::Participant::abortTransaction(OperationContext* opCtx) {
    // Normally, absence of a transaction resource stash indicates an inactive transaction.
    // However, in the case of a failed "unstash", an active transaction may exist without a stash
    // and be killed externally.  In that case, the opCtx will not have a transaction number.
    if (o().txnResourceStash || !opCtx->getTxnNumber()) {
        // Aborting an inactive transaction.
        _abortTransactionOnSession(opCtx);
    } else if (o().txnState.isPrepared()) {
        _abortActivePreparedTransaction(opCtx);
    } else {
        _abortActiveTransaction(opCtx, TransactionState::kInProgress);
    }
}

void TransactionParticipant::Participant::_abortActivePreparedTransaction(OperationContext* opCtx) {
    // Re-acquire the RSTL to prevent state transitions while aborting the transaction. Since the
    // transaction was prepared, we dropped it on preparing the transaction. We do not need to
    // reacquire the PBWM because if we're not the primary we will uassert anyways.
    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

    // Prepared transactions cannot hold the RSTL, or else they will deadlock with state
    // transitions. If we do not abort the transaction we must unlock the RSTL explicitly so two
    // phase locking doesn't hold onto it. Unlocking the RSTL may be a noop if it's already
    // unlocked.
    ON_BLOCK_EXIT([&] { opCtx->lockState()->unlockRSTLforPrepare(); });

    if (opCtx->writesAreReplicated()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassert(ErrorCodes::NotWritablePrimary,
                "Not primary so we cannot abort a prepared transaction",
                replCoord->canAcceptWritesForDatabase(opCtx, "admin"));
    }

    _abortActiveTransaction(opCtx, TransactionState::kPrepared);
}

void TransactionParticipant::Participant::_abortActiveTransaction(
    OperationContext* opCtx, TransactionState::StateSet expectedStates) {
    invariant(!o().txnResourceStash);

    if (!o().txnState.isInRetryableWriteMode()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).transactionMetricsObserver.onTransactionOperation(
            opCtx, CurOp::get(opCtx)->debug().additiveMetrics, o().txnState.isPrepared());
    }

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);

    const bool needToWriteAbortEntry = opCtx->writesAreReplicated() && p().needToWriteAbortEntry;
    if (needToWriteAbortEntry) {
        // We reserve an oplog slot before aborting the transaction so that no writes that are
        // causally related to the transaction abort enter the oplog at a timestamp earlier than the
        // abort oplog entry.
        OplogSlotReserver oplogSlotReserver(opCtx);

        // Clean up the transaction resources on the opCtx even if the transaction resources on the
        // session were not aborted. This actually aborts the storage-transaction.
        _cleanUpTxnResourceOnOpCtx(opCtx, TerminationCause::kAborted);

        try {
            // If we need to write an abort oplog entry, this function can no longer be interrupted.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // Write the abort oplog entry. This must be done after aborting the storage
            // transaction, so that the lock state is reset, and there is no max lock timeout on the
            // locker.
            opObserver->onTransactionAbort(opCtx, oplogSlotReserver.getLastSlot());

            _finishAbortingActiveTransaction(opCtx, expectedStates);
        } catch (...) {
            // It is illegal for aborting a transaction that must write an abort oplog entry to fail
            // after aborting the storage transaction, so we crash instead.
            severe()
                << "Caught exception during abort of transaction that must write abort oplog entry "
                << opCtx->getTxnNumber() << " on " << _sessionId().toBSON() << ": "
                << exceptionToStatus();
            std::terminate();
        }
    } else {
        // Clean up the transaction resources on the opCtx even if the transaction resources on the
        // session were not aborted. This actually aborts the storage-transaction.
        //
        // These functions are allowed to throw. We are not writing an oplog entry, so the only risk
        // is not cleaning up some internal TransactionParticipant state, updating metrics, or
        // logging the end of the transaction. That will either be cleaned up in the
        // ServiceEntryPoint's abortGuard or when the next transaction begins.
        _cleanUpTxnResourceOnOpCtx(opCtx, TerminationCause::kAborted);
        opObserver->onTransactionAbort(opCtx, boost::none);
        _finishAbortingActiveTransaction(opCtx, expectedStates);
    }
}

void TransactionParticipant::Participant::_finishAbortingActiveTransaction(
    OperationContext* opCtx, TransactionState::StateSet expectedStates) {
    // Only abort the transaction in session if it's in expected states.
    // When the state of active transaction on session is not expected, it means another
    // thread has already aborted the transaction on session.
    if (o().txnState.isInSet(expectedStates)) {
        invariant(opCtx->getTxnNumber() == o().activeTxnNumber);
        _abortTransactionOnSession(opCtx);
    } else if (opCtx->getTxnNumber() == o().activeTxnNumber) {
        if (o().txnState.isInRetryableWriteMode()) {
            // The active transaction is not a multi-document transaction.
            invariant(opCtx->getWriteUnitOfWork() == nullptr);
            return;
        }

        // Cannot abort these states unless they are specified in expectedStates explicitly.
        const auto unabortableStates = TransactionState::kPrepared  //
            | TransactionState::kCommitted;                         //
        invariant(!o().txnState.isInSet(unabortableStates),
                  str::stream() << "Cannot abort transaction in " << o().txnState);
    } else {
        // If _activeTxnNumber is higher than ours, it means the transaction is already aborted.
        invariant(o().txnState.isInSet(TransactionState::kNone |
                                       TransactionState::kAbortedWithoutPrepare |
                                       TransactionState::kAbortedWithPrepare |
                                       TransactionState::kExecutedRetryableWrite),
                  str::stream() << "actual state: " << o().txnState);
    }
}

void TransactionParticipant::Participant::_abortTransactionOnSession(OperationContext* opCtx) {
    const auto tickSource = opCtx->getServiceContext()->getTickSource();

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).transactionMetricsObserver.onAbort(
            ServerTransactionsMetrics::get(opCtx->getServiceContext()),
            tickSource,
            &Top::get(opCtx->getServiceContext()));
    }

    if (o().txnResourceStash) {
        _logSlowTransaction(opCtx,
                            &(o().txnResourceStash->locker()->getLockerInfo(boost::none))->stats,
                            TerminationCause::kAborted,
                            o().txnResourceStash->getReadConcernArgs());
    }

    const auto nextState = o().txnState.isPrepared() ? TransactionState::kAbortedWithPrepare
                                                     : TransactionState::kAbortedWithoutPrepare;

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    if (o().txnResourceStash && opCtx->recoveryUnit()->getNoEvictionAfterRollback()) {
        o(lk).txnResourceStash->setNoEvictionAfterRollback();
    }
    _resetTransactionStateAndUnlock(&lk, nextState);
}

void TransactionParticipant::Participant::_cleanUpTxnResourceOnOpCtx(
    OperationContext* opCtx, TerminationCause terminationCause) {
    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(
        opCtx,
        &(opCtx->lockState()->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase()))->stats,
        terminationCause,
        repl::ReadConcernArgs::get(opCtx));

    // Reset the WUOW. We should be able to abort empty transactions that don't have WUOW.
    if (opCtx->getWriteUnitOfWork()) {
        invariant(opCtx->lockState()->isRSTLLocked());
        opCtx->setWriteUnitOfWork(nullptr);
    }

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
}

void TransactionParticipant::Participant::_checkIsCommandValidWithTxnState(
    const TxnNumber& requestTxnNumber, const std::string& cmdName) const {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << requestTxnNumber << " has been aborted.",
            !o().txnState.isAborted());

    // Cannot change committed transaction but allow retrying commitTransaction command.
    uassert(ErrorCodes::TransactionCommitted,
            str::stream() << "Transaction " << requestTxnNumber << " has been committed.",
            cmdName == "commitTransaction" || !o().txnState.isCommitted());

    // Disallow operations other than abort, prepare or commit on a prepared transaction
    uassert(ErrorCodes::PreparedTransactionInProgress,
            str::stream() << "Cannot call any operation other than abort, prepare or commit on"
                          << " a prepared transaction",
            !o().txnState.isPrepared() ||
                preparedTxnCmdWhitelist.find(cmdName) != preparedTxnCmdWhitelist.cend());
}

BSONObj TransactionParticipant::Observer::reportStashedState(OperationContext* opCtx) const {
    BSONObjBuilder builder;
    reportStashedState(opCtx, &builder);
    return builder.obj();
}

void TransactionParticipant::Observer::reportStashedState(OperationContext* opCtx,
                                                          BSONObjBuilder* builder) const {
    if (o().txnResourceStash && o().txnResourceStash->locker()) {
        if (auto lockerInfo = o().txnResourceStash->locker()->getLockerInfo(boost::none)) {
            invariant(o().activeTxnNumber != kUninitializedTxnNumber);
            builder->append("type", "idleSession");
            builder->append("host", getHostNameCachedAndPort());
            builder->append("desc", "inactive transaction");

            const auto& lastClientInfo =
                o().transactionMetricsObserver.getSingleTransactionStats().getLastClientInfo();
            builder->append("client", lastClientInfo.clientHostAndPort);
            builder->append("connectionId", lastClientInfo.connectionId);
            builder->append("appName", lastClientInfo.appName);
            builder->append("clientMetadata", lastClientInfo.clientMetadata);

            {
                BSONObjBuilder lsid(builder->subobjStart("lsid"));
                _sessionId().serialize(&lsid);
            }

            BSONObjBuilder transactionBuilder;
            _reportTransactionStats(
                opCtx, &transactionBuilder, o().txnResourceStash->getReadConcernArgs());

            builder->append("transaction", transactionBuilder.obj());
            builder->append("waitingForLock", false);
            builder->append("active", false);

            fillLockerInfo(*lockerInfo, *builder);
        }
    }
}

void TransactionParticipant::Observer::reportUnstashedState(OperationContext* opCtx,
                                                            BSONObjBuilder* builder) const {
    // The Client mutex must be held when calling this function, so it is safe to access the state
    // of the TransactionParticipant.
    if (!o().txnResourceStash) {
        BSONObjBuilder transactionBuilder;
        _reportTransactionStats(opCtx, &transactionBuilder, repl::ReadConcernArgs::get(opCtx));
        builder->append("transaction", transactionBuilder.obj());
    }
}

std::string TransactionParticipant::TransactionState::toString(StateFlag state) {
    switch (state) {
        case TransactionParticipant::TransactionState::kNone:
            return "TxnState::None";
        case TransactionParticipant::TransactionState::kInProgress:
            return "TxnState::InProgress";
        case TransactionParticipant::TransactionState::kPrepared:
            return "TxnState::Prepared";
        case TransactionParticipant::TransactionState::kCommitted:
            return "TxnState::Committed";
        case TransactionParticipant::TransactionState::kAbortedWithoutPrepare:
            return "TxnState::AbortedWithoutPrepare";
        case TransactionParticipant::TransactionState::kAbortedWithPrepare:
            return "TxnState::AbortedAfterPrepare";
        case TransactionParticipant::TransactionState::kExecutedRetryableWrite:
            return "TxnState::ExecutedRetryableWrite";
    }
    MONGO_UNREACHABLE;
}

bool TransactionParticipant::TransactionState::_isLegalTransition(StateFlag oldState,
                                                                  StateFlag newState) {
    switch (oldState) {
        case kNone:
            switch (newState) {
                case kNone:
                case kInProgress:
                case kExecutedRetryableWrite:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kInProgress:
            switch (newState) {
                case kNone:
                case kPrepared:
                case kCommitted:
                case kAbortedWithoutPrepare:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kPrepared:
            switch (newState) {
                case kAbortedWithPrepare:
                case kCommitted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommitted:
            switch (newState) {
                case kNone:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kAbortedWithoutPrepare:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kAbortedWithPrepare:
            switch (newState) {
                case kNone:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kExecutedRetryableWrite:
            switch (newState) {
                case kNone:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

void TransactionParticipant::TransactionState::transitionTo(StateFlag newState,
                                                            TransitionValidation shouldValidate) {
    if (shouldValidate == TransitionValidation::kValidateTransition) {
        invariant(TransactionState::_isLegalTransition(_state, newState),
                  str::stream() << "Current state: " << toString(_state)
                                << ", Illegal attempted next state: " << toString(newState));
    }

    // If we are transitioning out of prepare, signal waiters by fulfilling the completion promise.
    if (isPrepared()) {
        invariant(_exitPreparePromise);
        _exitPreparePromise->emplaceValue();
        _exitPreparePromise.reset();
    }

    _state = newState;

    // If we have transitioned into prepare, set the completion promise so other threads can wait
    // on the participant to transition out of prepare.
    if (isPrepared()) {
        invariant(!_exitPreparePromise);
        _exitPreparePromise.emplace();
    }
}

void TransactionParticipant::Observer::_reportTransactionStats(
    OperationContext* opCtx, BSONObjBuilder* builder, repl::ReadConcernArgs readConcernArgs) const {
    const auto tickSource = opCtx->getServiceContext()->getTickSource();
    o().transactionMetricsObserver.getSingleTransactionStats().report(
        builder, readConcernArgs, tickSource, tickSource->getTicks());
}

std::string TransactionParticipant::Participant::_transactionInfoForLog(
    OperationContext* opCtx,
    const SingleThreadedLockStats* lockStats,
    TerminationCause terminationCause,
    repl::ReadConcernArgs readConcernArgs) const {
    invariant(lockStats);

    StringBuilder s;

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", o().activeTxnNumber);
    parametersBuilder.append("autocommit", p().autoCommit ? *p().autoCommit : true);
    readConcernArgs.appendInfo(&parametersBuilder);

    s << "parameters:" << parametersBuilder.obj().toString() << ",";

    const auto& singleTransactionStats = o().transactionMetricsObserver.getSingleTransactionStats();

    s << " readTimestamp:" << singleTransactionStats.getReadTimestamp().toString() << ",";

    s << singleTransactionStats.getOpDebug()->additiveMetrics.report();

    std::string terminationCauseString =
        terminationCause == TerminationCause::kCommitted ? "committed" : "aborted";
    s << " terminationCause:" << terminationCauseString;

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTick = tickSource->getTicks();

    s << " timeActiveMicros:"
      << durationCount<Microseconds>(
             singleTransactionStats.getTimeActiveMicros(tickSource, curTick));
    s << " timeInactiveMicros:"
      << durationCount<Microseconds>(
             singleTransactionStats.getTimeInactiveMicros(tickSource, curTick));

    // Number of yields is always 0 in multi-document transactions, but it is included mainly to
    // match the format with other slow operation logging messages.
    s << " numYields:" << 0;
    // Aggregate lock statistics.

    BSONObjBuilder locks;
    lockStats->report(&locks);
    s << " locks:" << locks.obj().toString();

    if (singleTransactionStats.getOpDebug()->storageStats)
        s << " storage:" << singleTransactionStats.getOpDebug()->storageStats->toBSON().toString();

    // It is possible for a slow transaction to have aborted in the prepared state if an
    // exception was thrown before prepareTransaction succeeds.
    const auto totalPreparedDuration = durationCount<Microseconds>(
        singleTransactionStats.getPreparedDuration(tickSource, curTick));
    const bool txnWasPrepared = totalPreparedDuration > 0;
    s << " wasPrepared:" << txnWasPrepared;
    if (txnWasPrepared) {
        s << " totalPreparedDurationMicros:" << totalPreparedDuration;
        s << " prepareOpTime:" << o().prepareOpTime.toString();
    }

    // Total duration of the transaction.
    s << ", "
      << duration_cast<Milliseconds>(singleTransactionStats.getDuration(tickSource, curTick));

    return s.str();
}

void TransactionParticipant::Participant::_logSlowTransaction(
    OperationContext* opCtx,
    const SingleThreadedLockStats* lockStats,
    TerminationCause terminationCause,
    repl::ReadConcernArgs readConcernArgs) {
    // Only log multi-document transactions.
    if (!o().txnState.isInRetryableWriteMode()) {
        const auto tickSource = opCtx->getServiceContext()->getTickSource();
        // Log the transaction if log message verbosity for transaction component is >= 1 or its
        // duration is longer than the slowMS command threshold.
        if (shouldLog(logger::LogComponent::kTransaction, logger::LogSeverity::Debug(1)) ||
            o().transactionMetricsObserver.getSingleTransactionStats().getDuration(
                tickSource, tickSource->getTicks()) > Milliseconds(serverGlobalParams.slowMS)) {
            log(logger::LogComponent::kTransaction)
                << "transaction "
                << _transactionInfoForLog(opCtx, lockStats, terminationCause, readConcernArgs);
        }
    }
}

void TransactionParticipant::Participant::_setNewTxnNumber(OperationContext* opCtx,
                                                           const TxnNumber& txnNumber) {
    uassert(ErrorCodes::PreparedTransactionInProgress,
            "Cannot change transaction number while the session has a prepared transaction",
            !o().txnState.isInSet(TransactionState::kPrepared));

    LOG_FOR_TRANSACTION(4) << "New transaction started with txnNumber: " << txnNumber
                           << " on session with lsid " << _sessionId().getId();

    // Abort the existing transaction if it's not prepared, committed, or aborted.
    if (o().txnState.isInProgress()) {
        _abortTransactionOnSession(opCtx);
    }

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    o(lk).activeTxnNumber = txnNumber;
    o(lk).lastWriteOpTime = repl::OpTime();

    // Reset the retryable writes state
    _resetRetryableWriteState();

    // Reset the transactions metrics
    o(lk).transactionMetricsObserver.resetSingleTransactionStats(txnNumber);

    // Reset the transactional state
    _resetTransactionStateAndUnlock(&lk, TransactionState::kNone);
}

void TransactionParticipant::Participant::refreshFromStorageIfNeeded(OperationContext* opCtx) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(!opCtx->lockState()->isLocked());

    if (p().isValid)
        return;

    auto activeTxnHistory = fetchActiveTransactionHistory(opCtx, _sessionId());
    const auto& lastTxnRecord = activeTxnHistory.lastTxnRecord;
    if (lastTxnRecord) {
        stdx::lock_guard<Client> lg(*opCtx->getClient());
        o(lg).activeTxnNumber = lastTxnRecord->getTxnNum();
        o(lg).lastWriteOpTime = lastTxnRecord->getLastWriteOpTime();
        p().activeTxnCommittedStatements = std::move(activeTxnHistory.committedStatements);
        p().hasIncompleteHistory = activeTxnHistory.hasIncompleteHistory;

        if (!lastTxnRecord->getState()) {
            o(lg).txnState.transitionTo(
                TransactionState::kExecutedRetryableWrite,
                TransactionState::TransitionValidation::kRelaxTransitionValidation);
        } else {
            switch (*lastTxnRecord->getState()) {
                case DurableTxnStateEnum::kCommitted:
                    o(lg).txnState.transitionTo(
                        TransactionState::kCommitted,
                        TransactionState::TransitionValidation::kRelaxTransitionValidation);
                    break;
                case DurableTxnStateEnum::kAborted:
                    o(lg).txnState.transitionTo(
                        TransactionState::kAbortedWithPrepare,
                        TransactionState::TransitionValidation::kRelaxTransitionValidation);
                    break;
                // We should never be refreshing a prepared or in-progress transaction from storage
                // since it should already be in a valid state after replication recovery.
                case DurableTxnStateEnum::kPrepared:
                case DurableTxnStateEnum::kInProgress:
                    MONGO_UNREACHABLE;
            }
        }
    }

    p().isValid = true;
}

void TransactionParticipant::Participant::onWriteOpCompletedOnPrimary(
    OperationContext* opCtx,
    std::vector<StmtId> stmtIdsWritten,
    const SessionTxnRecord& sessionTxnRecord) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    invariant(sessionTxnRecord.getSessionId() == _sessionId());
    invariant(sessionTxnRecord.getTxnNum() == o().activeTxnNumber);

    // Sanity check that we don't double-execute statements
    for (const auto stmtId : stmtIdsWritten) {
        const auto stmtOpTime = _checkStatementExecuted(stmtId);
        if (stmtOpTime) {
            fassertOnRepeatedExecution(_sessionId(),
                                       sessionTxnRecord.getTxnNum(),
                                       stmtId,
                                       *stmtOpTime,
                                       sessionTxnRecord.getLastWriteOpTime());
        }
    }

    const auto updateRequest = _makeUpdateRequest(sessionTxnRecord);

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(
        opCtx, std::move(stmtIdsWritten), sessionTxnRecord.getLastWriteOpTime());
}

void TransactionParticipant::Participant::onMigrateCompletedOnPrimary(
    OperationContext* opCtx,
    std::vector<StmtId> stmtIdsWritten,
    const SessionTxnRecord& sessionTxnRecord) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    invariant(sessionTxnRecord.getSessionId() == _sessionId());
    invariant(sessionTxnRecord.getTxnNum() == o().activeTxnNumber);

    const auto updateRequest = _makeUpdateRequest(sessionTxnRecord);

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(
        opCtx, std::move(stmtIdsWritten), sessionTxnRecord.getLastWriteOpTime());
}

void TransactionParticipant::Participant::_invalidate(WithLock wl) {
    p().isValid = false;
    o(wl).activeTxnNumber = kUninitializedTxnNumber;
    o(wl).lastWriteOpTime = repl::OpTime();

    // Reset the transactions metrics.
    o(wl).transactionMetricsObserver.resetSingleTransactionStats(o().activeTxnNumber);
}

void TransactionParticipant::Participant::_resetRetryableWriteState() {
    p().activeTxnCommittedStatements.clear();
    p().hasIncompleteHistory = false;
}

void TransactionParticipant::Participant::_resetTransactionStateAndUnlock(
    stdx::unique_lock<Client>* lk, TransactionState::StateFlag state) {
    invariant(lk && lk->owns_lock());

    // If we are transitioning to kNone, we are either starting a new transaction or aborting a
    // prepared transaction for rollback. In the latter case, we will need to relax the invariant
    // that prevents transitioning from kPrepared to kNone.
    if (o().txnState.isPrepared() && state == TransactionState::kNone) {
        o(*lk).txnState.transitionTo(
            state, TransactionState::TransitionValidation::kRelaxTransitionValidation);
    } else {
        o(*lk).txnState.transitionTo(state);
    }

    p().transactionOperationBytes = 0;
    p().transactionOperations.clear();
    o(*lk).prepareOpTime = repl::OpTime();
    o(*lk).recoveryPrepareOpTime = repl::OpTime();
    p().multikeyPathInfo.clear();
    p().autoCommit = boost::none;
    p().needToWriteAbortEntry = false;

    // Swap out txnResourceStash while holding the Client lock, then release any locks held by this
    // participant and abort the storage transaction after releasing the lock. The transaction
    // rollback can block indefinitely if the storage engine recruits it for eviction. In that case
    // we should not be holding the Client lock, as that would block tasks like the periodic
    // transaction killer from making progress.
    using std::swap;
    boost::optional<TxnResources> temporary;
    swap(o(*lk).txnResourceStash, temporary);
    lk->unlock();
    temporary = boost::none;
}

void TransactionParticipant::Participant::invalidate(OperationContext* opCtx) {
    stdx::unique_lock<Client> lk(*opCtx->getClient());

    uassert(ErrorCodes::PreparedTransactionInProgress,
            "Cannot invalidate prepared transaction",
            !o().txnState.isInSet(TransactionState::kPrepared));

    // Invalidate the session and clear both the retryable writes and transactional states on
    // this participant.
    _invalidate(lk);
    _resetRetryableWriteState();
    _resetTransactionStateAndUnlock(&lk, TransactionState::kNone);
}

boost::optional<repl::OplogEntry> TransactionParticipant::Participant::checkStatementExecuted(
    OperationContext* opCtx, StmtId stmtId) const {
    const auto stmtTimestamp = _checkStatementExecuted(stmtId);

    if (!stmtTimestamp)
        return boost::none;

    TransactionHistoryIterator txnIter(*stmtTimestamp);
    while (txnIter.hasNext()) {
        const auto entry = txnIter.next(opCtx);
        invariant(entry.getStatementId());
        if (*entry.getStatementId() == stmtId)
            return entry;
    }

    MONGO_UNREACHABLE;
}

bool TransactionParticipant::Participant::checkStatementExecutedNoOplogEntryFetch(
    StmtId stmtId) const {
    return bool(_checkStatementExecuted(stmtId));
}

boost::optional<repl::OpTime> TransactionParticipant::Participant::_checkStatementExecuted(
    StmtId stmtId) const {
    invariant(p().isValid);

    const auto it = p().activeTxnCommittedStatements.find(stmtId);
    if (it == p().activeTxnCommittedStatements.end()) {
        uassert(ErrorCodes::IncompleteTransactionHistory,
                str::stream() << "Incomplete history detected for transaction "
                              << o().activeTxnNumber << " on session " << _sessionId(),
                !p().hasIncompleteHistory);

        return boost::none;
    }

    return it->second;
}

UpdateRequest TransactionParticipant::Participant::_makeUpdateRequest(
    const SessionTxnRecord& sessionTxnRecord) const {
    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);

    updateRequest.setUpdateModification(sessionTxnRecord.toBSON());
    updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName << _sessionId().toBSON()));
    updateRequest.setUpsert(true);

    return updateRequest;
}

void TransactionParticipant::Participant::_registerUpdateCacheOnCommit(
    OperationContext* opCtx,
    std::vector<StmtId> stmtIdsWritten,
    const repl::OpTime& lastStmtIdWriteOpTime) {
    opCtx->recoveryUnit()->onCommit([opCtx,
                                     stmtIdsWritten = std::move(stmtIdsWritten),
                                     lastStmtIdWriteOpTime](boost::optional<Timestamp>) {
        TransactionParticipant::Participant participant(opCtx);
        invariant(participant.p().isValid);

        RetryableWritesStats::get(opCtx->getServiceContext())
            ->incrementTransactionsCollectionWriteCount();

        stdx::lock_guard<Client> lg(*opCtx->getClient());

        // The cache of the last written record must always be advanced after a write so that
        // subsequent writes have the correct point to start from.
        participant.o(lg).lastWriteOpTime = lastStmtIdWriteOpTime;

        for (const auto stmtId : stmtIdsWritten) {
            if (stmtId == kIncompleteHistoryStmtId) {
                participant.p().hasIncompleteHistory = true;
                continue;
            }

            const auto insertRes =
                participant.p().activeTxnCommittedStatements.emplace(stmtId, lastStmtIdWriteOpTime);
            if (!insertRes.second) {
                const auto& existingOpTime = insertRes.first->second;
                fassertOnRepeatedExecution(participant._sessionId(),
                                           participant.o().activeTxnNumber,
                                           stmtId,
                                           existingOpTime,
                                           lastStmtIdWriteOpTime);
            }
        }

        // If this is the first time executing a retryable write, we should indicate that to
        // the transaction participant.
        if (participant.o(lg).txnState.isNone()) {
            participant.o(lg).txnState.transitionTo(TransactionState::kExecutedRetryableWrite);
        }
    });

    MONGO_FAIL_POINT_BLOCK(onPrimaryTransactionalWrite, customArgs) {
        const auto& data = customArgs.getData();

        const auto closeConnectionElem = data["closeConnection"];
        if (closeConnectionElem.eoo() || closeConnectionElem.Bool()) {
            opCtx->getClient()->session()->end();
        }

        const auto failBeforeCommitExceptionElem = data["failBeforeCommitExceptionCode"];
        if (!failBeforeCommitExceptionElem.eoo()) {
            const auto failureCode = ErrorCodes::Error(int(failBeforeCommitExceptionElem.Number()));
            uasserted(failureCode,
                      str::stream()
                          << "Failing write for " << _sessionId() << ":" << o().activeTxnNumber
                          << " due to failpoint. The write must not be reflected.");
        }
    }
}

}  // namespace mongo
