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

#define LOGV2_FOR_TRANSACTION(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kTransaction}, MESSAGE, ##__VA_ARGS__)

#include "mongo/db/transaction/transaction_participant.h"

#include <fmt/format.h>

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/server_transactions_metrics.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/txn_retry_counter_too_old_info.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_with_sampling.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

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

MONGO_FAIL_POINT_DEFINE(failTransactionNoopWrite);

const auto getTransactionParticipant = Session::declareDecoration<TransactionParticipant>();

const auto retryableWriteTransactionParticipantCatalogDecoration =
    Session::declareDecoration<RetryableWriteTransactionParticipantCatalog>();

/**
 * Returns the RetryableWriteTransactionParticipantCatalog for the given session.
 */
RetryableWriteTransactionParticipantCatalog& getRetryableWriteTransactionParticipantCatalog(
    Session* session) {
    if (const auto parentSession = session->getParentSession()) {
        return retryableWriteTransactionParticipantCatalogDecoration(parentSession);
    }
    return retryableWriteTransactionParticipantCatalogDecoration(session);
}

/**
 * Returns the RetryableWriteTransactionParticipantCatalog for the session checked out by the
 * given 'opCtx'.
 */
RetryableWriteTransactionParticipantCatalog& getRetryableWriteTransactionParticipantCatalog(
    OperationContext* opCtx) {
    auto session = OperationContextSession::get(opCtx);
    return getRetryableWriteTransactionParticipantCatalog(session);
}

// The command names that are allowed in a prepared transaction.
const StringMap<int> preparedTxnCmdAllowlist = {
    {"abortTransaction", 1}, {"commitTransaction", 1}, {"prepareTransaction", 1}};

void fassertOnRepeatedExecution(const LogicalSessionId& lsid,
                                TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                StmtId stmtId,
                                const repl::OpTime& firstOpTime,
                                const repl::OpTime& secondOpTime) {
    LOGV2_FATAL(
        40526,
        "Statement id {stmtId} from transaction [ {lsid}:{txnNumberAndRetryCounter} ] was "
        "committed once with opTime {firstCommitOpTime} and a second time with opTime { "
        "secondCommitOpTime}. This indicates possible data corruption or server bug and the "
        "process will be terminated.",
        "Statement from transaction was committed twice. This indicates possible data corruption "
        "or server bug and the process will be terminated",
        "stmtId"_attr = stmtId,
        "lsid"_attr = lsid.toBSON(),
        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
        "firstCommitOpTime"_attr = firstOpTime,
        "secondCommitOpTime"_attr = secondOpTime);
}

void validateTransactionHistoryApplyOpsOplogEntry(const repl::OplogEntry& oplogEntry) {
    uassert(5875601,
            "Found an applyOps oplog entry for retryable writes that were executed without "
            "using a retryable internal transaction",
            isInternalSessionForRetryableWrite(*oplogEntry.getSessionId()));
    uassert(5875602,
            "Found an applyOps oplog entry for retryable internal transaction with top-level "
            "'stmtId' field",
            oplogEntry.getStatementIds().empty());
}

/**
 * Runs the given 'callable' with a DBDirectClient with a no-timestamp read source, and restores
 * the original timestamp read source after returning. Used for performing a read against the
 * config.transactions collection during refresh below since snapshot reads and causal consistent
 * majority reads against are not supported in that collection.
 */
template <typename Callable>
auto performReadWithNoTimestampDBDirectClient(OperationContext* opCtx, Callable&& callable) {
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);

    DBDirectClient client(opCtx);
    // If the 'opCtx' is marked as "in multi document transaction", the read done by 'callable'
    // would acquire the global lock in the IX mode. That upconvert would require a flow control
    // ticket to be obtained.
    FlowControl::Bypass flowControlBypass(opCtx);
    return callable(&client);
}

void rethrowPartialIndexQueryBadValueWithContext(const DBException& ex) {
    if (ex.reason().find("hint provided does not correspond to an existing index") !=
        std::string::npos) {
        uassertStatusOKWithContext(
            ex.toStatus(),
            str::stream()
                << "Failed to find partial index for "
                << NamespaceString::kSessionTransactionsTableNamespace.ns()
                << ". Please create an index directly on this replica set with the specification: "
                << MongoDSessionCatalog::getConfigTxnPartialIndexSpec() << " or drop the "
                << NamespaceString::kSessionTransactionsTableNamespace.ns()
                << " collection and step up a new primary.");
    }
}

struct ActiveTransactionHistory {
    boost::optional<SessionTxnRecord> lastTxnRecord;
    TransactionParticipant::CommittedStatementTimestampMap committedStatements;
    bool hasIncompleteHistory{false};
};

ActiveTransactionHistory fetchActiveTransactionHistory(OperationContext* opCtx,
                                                       const LogicalSessionId& lsid,
                                                       bool fetchOplogEntries) {
    // FlowControl is only impacted when a MODE_IX global lock is acquired. If we are in a
    // multi-document transaction, we must acquire a MODE_IX global lock. Prevent obtaining a flow
    // control ticket while in a mutli-document transaction.
    FlowControl::Bypass flowControlBypass(opCtx);

    // Storage engine operations require at a least global MODE_IS lock. In multi-document
    // transactions, storage opeartions require at least a global MODE_IX lock. Prevent lock
    // upgrading in the case of a multi-document transaction.
    Lock::GlobalLock lk(opCtx, opCtx->inMultiDocumentTransaction() ? MODE_IX : MODE_IS);

    ActiveTransactionHistory result;

    result.lastTxnRecord = [&]() -> auto {
        return performReadWithNoTimestampDBDirectClient(
            opCtx, [&](DBDirectClient* client) -> boost::optional<SessionTxnRecord> {
                auto result =
                    client->findOne(NamespaceString::kSessionTransactionsTableNamespace,
                                    BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON()));
                if (result.isEmpty()) {
                    return boost::none;
                }
                return SessionTxnRecord::parse(
                    IDLParserContext("parse latest txn record for session"), result);
            });
    }
    ();

    if (!result.lastTxnRecord) {
        return result;
    }

    if (auto state = result.lastTxnRecord->getState()) {
        if (!isInternalSessionForRetryableWrite(lsid) || state != DurableTxnStateEnum::kCommitted) {
            // When state is given, it must be a transaction, so we don't need to traverse the
            // history if it is not a committed transaction for retryable writes.
            return result;
        }
    }

    if (!fetchOplogEntries) {
        return result;
    }

    // Helper for registering statement ids of an oplog entry for a retryable write or a retryable
    // internal transaction.
    auto insertStmtIdsForOplogEntry = [&](const repl::OplogEntry& entry) {
        for (auto stmtId : entry.getStatementIds()) {
            uassert(5875604,
                    str::stream() << "Found an oplog entry with an invalid stmtId "
                                  << entry.toBSONForLogging(),
                    stmtId >= 0);
            const auto insertRes = result.committedStatements.emplace(stmtId, entry.getOpTime());
            if (!insertRes.second) {
                const auto& existingOpTime = insertRes.first->second;
                fassertOnRepeatedExecution(lsid,
                                           result.lastTxnRecord->getTxnNum(),
                                           stmtId,
                                           existingOpTime,
                                           entry.getOpTime());
            }
        }
    };

    // Restore the current timestamp read source after fetching transaction history, which may
    // change our ReadSource.
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);

    auto it = TransactionHistoryIterator(result.lastTxnRecord->getLastWriteOpTime());
    while (it.hasNext()) {
        try {
            const auto entry = it.next(opCtx);

            auto stmtIds = entry.getStatementIds();

            if (isInternalSessionForRetryableWrite(lsid)) {
                uassert(5875605,
                        "Found an oplog entry for retryable internal transaction with top-level "
                        "'stmtId' field",
                        stmtIds.empty());

                if (entry.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction) {
                    continue;
                } else if (entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps) {
                    validateTransactionHistoryApplyOpsOplogEntry(entry);

                    std::vector<repl::OplogEntry> innerEntries;
                    repl::ApplyOps::extractOperationsTo(
                        entry, entry.getEntry().toBSON(), &innerEntries);
                    for (const auto& innerEntry : innerEntries) {
                        insertStmtIdsForOplogEntry(innerEntry);
                    }
                } else {
                    MONGO_UNREACHABLE;
                }
            } else {
                // Oplog entries for retryable writes are expected to have a statement id.
                invariant(!stmtIds.empty());

                if (stmtIds.front() == kIncompleteHistoryStmtId) {
                    // Only the dead end sentinel can have this id for oplog write history
                    invariant(stmtIds.size() == 1);
                    invariant(entry.getObject2());
                    invariant(entry.getObject2()->woCompare(
                                  TransactionParticipant::kDeadEndSentinel) == 0);
                    result.hasIncompleteHistory = true;
                    continue;
                }

                // TODO (SERVER-64172): Remove leftover upgrade/downgrade code from 4.2 in
                // fetchActiveTransactionHistory.
                if (entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps &&
                    !entry.shouldPrepare() && !entry.isPartialTransaction()) {
                    result.lastTxnRecord->setState(DurableTxnStateEnum::kCommitted);
                    return result;
                }

                insertStmtIdsForOplogEntry(entry);
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

/**
 * Returns the highest txnNumber in the given session that has corresponding internal sessions as
 * found in the config.transactions collection.
 */
TxnNumber fetchHighestTxnNumberWithInternalSessions(OperationContext* opCtx,
                                                    const LogicalSessionId& parentLsid) {
    TxnNumber highestTxnNumber = kUninitializedTxnNumber;
    try {
        performReadWithNoTimestampDBDirectClient(opCtx, [&](DBDirectClient* client) {
            FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
            findRequest.setFilter(
                BSON(SessionTxnRecord::kParentSessionIdFieldName << parentLsid.toBSON()));
            findRequest.setSort(BSON((SessionTxnRecord::kSessionIdFieldName + "." +
                                      LogicalSessionId::kTxnNumberFieldName)
                                     << -1));
            findRequest.setProjection(BSON(SessionTxnRecord::kSessionIdFieldName << 1));
            findRequest.setLimit(1);
            findRequest.setHint(BSON("$hint" << MongoDSessionCatalog::kConfigTxnsPartialIndexName));

            auto cursor = client->find(findRequest);

            while (cursor->more()) {
                const auto doc = cursor->next();
                const auto childLsid = LogicalSessionId::parse(IDLParserContext("LogicalSessionId"),
                                                               doc.getObjectField("_id"));

                invariant(!cursor->more());
                // All config.transactions entries with the parentLsid field should have a txnNumber
                // in their sessionId, but users may manually modify that collection so we can't
                // assume that.
                if (childLsid.getTxnNumber().has_value()) {
                    highestTxnNumber = *childLsid.getTxnNumber();
                }
            }
        });
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        rethrowPartialIndexQueryBadValueWithContext(ex);
        throw;
    }

    return highestTxnNumber;
}

void updateSessionEntry(OperationContext* opCtx,
                        const UpdateRequest& updateRequest,
                        const LogicalSessionId& sessionId,
                        TxnNumber txnNum) {
    // Current code only supports replacement update.
    dassert(updateRequest.getUpdateModification().type() ==
            write_ops::UpdateModification::Type::kReplacement);
    const auto updateMod = updateRequest.getUpdateModification().getUpdateReplacement();

    // TODO SERVER-58243: evaluate whether this is safe or whether acquiring the lock can block.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    AutoGetCollection collection(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);

    uassert(40527,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            collection.getCollection());

    WriteUnitOfWork wuow(opCtx);

    auto idIndex = collection->getIndexCatalog()->findIdIndex(opCtx);

    uassert(40672,
            str::stream() << "Failed to fetch _id index for "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns(),
            idIndex);

    auto indexAccess =
        collection->getIndexCatalog()->getEntry(idIndex)->accessMethod()->asSortedData();
    // Since we are looking up a key inside the _id index, create a key object consisting of only
    // the _id field.
    auto idToFetch = updateRequest.getQuery().firstElement();
    auto toUpdateIdDoc = idToFetch.wrap();
    dassert(idToFetch.fieldNameStringData() == "_id"_sd);
    auto recordId = indexAccess->findSingle(opCtx, *collection, toUpdateIdDoc);
    auto startingSnapshotId = opCtx->recoveryUnit()->getSnapshotId();

    if (recordId.isNull()) {
        // Upsert case.
        auto status = collection_internal::insertDocument(
            opCtx, *collection, InsertStatement(updateMod), nullptr, false);

        if (status == ErrorCodes::DuplicateKey) {
            throwWriteConflictException(
                str::stream() << "Updating session entry failed with duplicate key, session "_sd
                              << sessionId << ", transaction "_sd << txnNum);
        }

        uassertStatusOK(status);
        wuow.commit();
        return;
    }

    auto originalRecordData = collection->getRecordStore()->dataFor(opCtx, recordId);
    auto originalDoc = originalRecordData.toBson();

    const auto parentLsidFieldName = SessionTxnRecord::kParentSessionIdFieldName;
    uassert(5875700,
            str::stream() << "Cannot modify the '" << parentLsidFieldName << "' field of "
                          << NamespaceString::kSessionTransactionsTableNamespace << " entries",
            updateMod.getObjectField(parentLsidFieldName)
                    .woCompare(originalDoc.getObjectField(parentLsidFieldName)) == 0);

    invariant(collection->getDefaultCollator() == nullptr);
    boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx, nullptr, updateRequest.getNamespaceString()));

    auto matcher =
        fassert(40673, MatchExpressionParser::parse(updateRequest.getQuery(), std::move(expCtx)));
    if (!matcher->matchesBSON(originalDoc)) {
        // Document no longer match what we expect so throw WCE to make the caller re-examine.
        throwWriteConflictException(
            str::stream() << "Updating session entry failed as document no longer matches, "_sd
                          << "session "_sd << sessionId << ", transaction "_sd << txnNum);
    }

    CollectionUpdateArgs args;
    args.update = updateMod;
    args.criteria = toUpdateIdDoc;

    // Specify indexesAffected = false because the sessions collection has two indexes: {_id: 1} and
    // {parentLsid: 1, _id.txnNumber: 1, _id: 1}, and none of the fields are mutable.
    collection->updateDocument(opCtx,
                               recordId,
                               Snapshotted<BSONObj>(startingSnapshotId, originalDoc),
                               updateMod,
                               false, /* indexesAffected */
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

TransactionParticipant::Participant::Participant(OperationContext* opCtx, Session* session)
    : Observer([opCtx, session]() -> TransactionParticipant* {
          invariant(session);

          auto checkedOutSession = OperationContextSession::get(opCtx);
          uassert(6202000,
                  str::stream() << "Cannot get the transaction participant for the session "
                                << session->getSessionId()
                                << " without having it or its parent checked out",
                  checkedOutSession &&
                      (castToParentSessionId(checkedOutSession->getSessionId()) ==
                       castToParentSessionId(session->getSessionId())));
          return &getTransactionParticipant(session);
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

    // Simulate an operation timeout and fail the noop write if the fail point is enabled. This is
    // to test that NoSuchTransaction error is not considered transient if the noop write cannot
    // occur.
    if (MONGO_unlikely(failTransactionNoopWrite.shouldFail())) {
        uasserted(ErrorCodes::MaxTimeMSExpired, "failTransactionNoopWrite fail point enabled");
    }

    {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
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
        Lock::DBLock dbLock(opCtx.get(), nss.dbName(), MODE_IS, deadline);
        Lock::CollectionLock collLock(opCtx.get(), nss, MODE_IS, deadline);

        auto databaseHolder = DatabaseHolder::get(opCtx.get());
        auto db = databaseHolder->getDb(opCtx.get(), nss.dbName());
        if (!db) {
            // There is no config database, so there cannot be any active transactions.
            return boost::none;
        }

        auto collection =
            CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
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
            auto doc = record.value().data.toBson();
            auto txnRecord =
                SessionTxnRecord::parse(IDLParserContext("parse oldest active txn record"), doc);
            if (txnRecord.getState() != DurableTxnStateEnum::kPrepared &&
                txnRecord.getState() != DurableTxnStateEnum::kInProgress) {
                continue;
            }
            // A prepared transaction must have a start timestamp.
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

boost::optional<TxnNumber> TransactionParticipant::Observer::getClientTxnNumber(
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) const {
    if (_isInternalSessionForNonRetryableWrite()) {
        return boost::none;
    } else if (_isInternalSessionForRetryableWrite()) {
        invariant(_sessionId().getTxnNumber());
        return _sessionId().getTxnNumber();
    }
    return {txnNumberAndRetryCounter.getTxnNumber()};
}

Session* TransactionParticipant::Observer::_session() const {
    return getTransactionParticipant.owner(_tp);
}

const LogicalSessionId& TransactionParticipant::Observer::_sessionId() const {
    return _session()->getSessionId();
}

bool TransactionParticipant::Observer::_isInternalSession() const {
    return isChildSession(_sessionId());
}

bool TransactionParticipant::Observer::_isInternalSessionForRetryableWrite() const {
    return isInternalSessionForRetryableWrite(_sessionId());
}

bool TransactionParticipant::Observer::_isInternalSessionForNonRetryableWrite() const {
    return isInternalSessionForNonRetryableWrite(_sessionId());
}

boost::optional<TxnNumber> TransactionParticipant::Observer::_activeRetryableWriteTxnNumber()
    const {
    if (_isInternalSessionForNonRetryableWrite()) {
        return boost::none;
    }

    if (_isInternalSessionForRetryableWrite()) {
        return *_sessionId().getTxnNumber();
    }

    invariant(!_isInternalSession());
    if (o().txnState.isInRetryableWriteMode()) {
        const auto txnNumber = o().activeTxnNumberAndRetryCounter.getTxnNumber();
        return txnNumber != kUninitializedTxnNumber ? boost::make_optional(txnNumber) : boost::none;
    }
    return boost::none;
}

void TransactionParticipant::Participant::_uassertNoConflictingInternalTransactionForRetryableWrite(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    auto clientTxnNumber = getClientTxnNumber(txnNumberAndRetryCounter);
    if (!clientTxnNumber) {
        // This must be a non-retryable child session transaction so there can't be a conflict.
        return;
    }

    auto& retryableWriteTxnParticipantCatalog =
        getRetryableWriteTransactionParticipantCatalog(opCtx);
    retryableWriteTxnParticipantCatalog.checkForConflictingInternalTransactions(
        opCtx, *clientTxnNumber, txnNumberAndRetryCounter);
}

bool TransactionParticipant::Participant::_verifyCanBeginMultiDocumentTransaction(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    if (txnNumberAndRetryCounter.getTxnNumber() ==
        o().activeTxnNumberAndRetryCounter.getTxnNumber()) {
        if (txnNumberAndRetryCounter.getTxnRetryCounter() <
            o().activeTxnNumberAndRetryCounter.getTxnRetryCounter()) {
            uasserted(
                TxnRetryCounterTooOldInfo(*o().activeTxnNumberAndRetryCounter.getTxnRetryCounter()),
                str::stream() << "Cannot start a transaction at given transaction number "
                              << txnNumberAndRetryCounter.getTxnNumber() << " on session "
                              << _sessionId() << " using txnRetryCounter "
                              << txnNumberAndRetryCounter.getTxnRetryCounter()
                              << " because it has already been restarted using a "
                              << "higher txnRetryCounter "
                              << o().activeTxnNumberAndRetryCounter.getTxnRetryCounter());
        } else if (txnNumberAndRetryCounter.getTxnRetryCounter() ==
                       o().activeTxnNumberAndRetryCounter.getTxnRetryCounter() ||
                   o().activeTxnNumberAndRetryCounter.getTxnRetryCounter() ==
                       kUninitializedTxnRetryCounter) {
            // Servers in a sharded cluster can start a new transaction at the active transaction
            // number to allow internal retries by routers on re-targeting errors, like
            // StaleShard/DatabaseVersion or SnapshotTooOld.
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Only servers in a sharded cluster can start a new transaction at the active "
                    "transaction number",
                    serverGlobalParams.clusterRole != ClusterRole::None);

            if (_isInternalSessionForRetryableWrite() &&
                o().txnState.isInSet(TransactionState::kCommitted)) {
                // This is a retry of a committed internal transaction for retryable writes so
                // skip resetting the state and updating the metrics.
                return true;
            }

            _uassertCanReuseActiveTxnNumberForTransaction(opCtx);
        } else {
            const auto restartableStates = TransactionState::kNone | TransactionState::kInProgress |
                TransactionState::kAbortedWithoutPrepare | TransactionState::kAbortedWithPrepare;
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Cannot restart transaction "
                                  << txnNumberAndRetryCounter.getTxnNumber()
                                  << " using txnRetryCounter "
                                  << txnNumberAndRetryCounter.getTxnRetryCounter()
                                  << " because it is already in state " << o().txnState
                                  << " with txnRetryCounter "
                                  << o().activeTxnNumberAndRetryCounter.getTxnRetryCounter(),
                    o().txnState.isInSet(restartableStates));
        }
    } else {
        invariant(txnNumberAndRetryCounter.getTxnNumber() >
                  o().activeTxnNumberAndRetryCounter.getTxnNumber());
    }

    _uassertNoConflictingInternalTransactionForRetryableWrite(opCtx, txnNumberAndRetryCounter);
    return false;
}

void TransactionParticipant::Participant::_uassertCanReuseActiveTxnNumberForTransaction(
    OperationContext* opCtx) {
    if (o().txnState.isInSet(TransactionState::kNone)) {
        const auto& retryableWriteTxnParticipantCatalog =
            getRetryableWriteTransactionParticipantCatalog(opCtx);
        invariant(retryableWriteTxnParticipantCatalog.isValid());

        for (const auto& it : retryableWriteTxnParticipantCatalog.getParticipants()) {
            const auto& txnParticipant = it.second;

            if (txnParticipant._sessionId() == _sessionId()) {
                continue;
            }

            invariant(txnParticipant._isInternalSessionForRetryableWrite());
            uassert(
                6202002,
                str::stream() << "Cannot start transaction with session id " << _sessionId()
                              << " and transaction number "
                              << o().activeTxnNumberAndRetryCounter.getTxnNumber()
                              << " because a retryable write with the same transaction number"
                              << " is being executed in a retryable internal transaction "
                              << " with session id " << txnParticipant._sessionId()
                              << " and transaction number "
                              << txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber()
                              << " in state " << txnParticipant.o().txnState,
                txnParticipant.transactionIsAbortedWithoutPrepare());
        }
    } else {
        uassert(
            50911,
            str::stream() << "Cannot start a transaction with session id " << _sessionId()
                          << " and transaction number "
                          << o().activeTxnNumberAndRetryCounter.toBSON()
                          << " because a transaction with the same transaction number is in state "
                          << o().txnState,
            o().txnState.isInSet(TransactionState::kAbortedWithoutPrepare));
    }
}

void TransactionParticipant::Participant::_beginOrContinueRetryableWrite(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    invariant(!txnNumberAndRetryCounter.getTxnRetryCounter());

    _uassertNoConflictingInternalTransactionForRetryableWrite(opCtx, txnNumberAndRetryCounter);

    if (txnNumberAndRetryCounter.getTxnNumber() >
        o().activeTxnNumberAndRetryCounter.getTxnNumber()) {
        // New retryable write.
        _setNewTxnNumberAndRetryCounter(
            opCtx, {txnNumberAndRetryCounter.getTxnNumber(), kUninitializedTxnRetryCounter});
        p().autoCommit = boost::none;

        auto& retryableWriteTxnParticipantCatalog =
            getRetryableWriteTransactionParticipantCatalog(opCtx);
        retryableWriteTxnParticipantCatalog.addParticipant(*this);
    } else {
        // Retrying a retryable write.

        // If this retryable write's transaction id has been converted to a transaction, and that
        // transaction is in prepare, wait for it to exit prepare before throwing
        // IncompleteTransactionHistory so the error response's operationTime is inclusive of the
        // transaction's 2PC decision, guaranteeing causally consistent sessions will always read
        // the transaction's writes.
        uassert(ErrorCodes::PreparedTransactionInProgress,
                "Retryable write that has been converted to a transaction is in prepare",
                !o().txnState.isInSet(TransactionState::kPrepared));

        uassert(ErrorCodes::IncompleteTransactionHistory,
                "Cannot retry a retryable write that has been converted into a transaction",
                o().txnState.isInRetryableWriteMode());
        invariant(p().autoCommit == boost::none);
    }
}

void TransactionParticipant::Participant::_continueMultiDocumentTransaction(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream()
                << "Given transaction number " << txnNumberAndRetryCounter.getTxnNumber()
                << " does not match any in-progress transactions. The active transaction number is "
                << o().activeTxnNumberAndRetryCounter.getTxnNumber(),
            txnNumberAndRetryCounter.getTxnNumber() ==
                    o().activeTxnNumberAndRetryCounter.getTxnNumber() &&
                !o().txnState.isInRetryableWriteMode());

    uassert(TxnRetryCounterTooOldInfo(*o().activeTxnNumberAndRetryCounter.getTxnRetryCounter()),
            str::stream() << "Cannot continue transaction "
                          << txnNumberAndRetryCounter.getTxnNumber() << " on session "
                          << _sessionId() << " using txnRetryCounter "
                          << txnNumberAndRetryCounter.getTxnRetryCounter()
                          << " because it has already been restarted using a higher"
                          << " txnRetryCounter "
                          << o().activeTxnNumberAndRetryCounter.getTxnRetryCounter(),
            txnNumberAndRetryCounter.getTxnRetryCounter() >=
                o().activeTxnNumberAndRetryCounter.getTxnRetryCounter());
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Cannot continue transaction "
                          << txnNumberAndRetryCounter.getTxnNumber() << " on session "
                          << _sessionId() << " using txnNumberAndRetryCounter.getTxnRetryCounter() "
                          << txnNumberAndRetryCounter.getTxnRetryCounter()
                          << " because it is currently in state " << o().txnState
                          << " with txnRetryCounter "
                          << o().activeTxnNumberAndRetryCounter.getTxnRetryCounter(),
            txnNumberAndRetryCounter.getTxnRetryCounter() ==
                o().activeTxnNumberAndRetryCounter.getTxnRetryCounter());

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
                << "Transaction with " << txnNumberAndRetryCounter.toBSON()
                << " has been aborted because an earlier command in this transaction failed.");
    }
}

void TransactionParticipant::Participant::_beginMultiDocumentTransaction(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    // Aborts any in-progress txns.
    _setNewTxnNumberAndRetryCounter(opCtx, txnNumberAndRetryCounter);
    p().autoCommit = false;

    auto& retryableWriteTxnParticipantCatalog =
        getRetryableWriteTransactionParticipantCatalog(opCtx);
    if (_isInternalSessionForRetryableWrite()) {
        retryableWriteTxnParticipantCatalog.addParticipant(*this);
    } else if (!_isInternalSessionForNonRetryableWrite()) {
        // Don't reset the RetryableWriteTransactionParticipantCatalog upon starting an internal
        // transaction for a non-retryable write since the transaction is unrelated to the
        // retryable write or transaction in the original session that the write runs in. In
        // addition, it is incorrect to clear the transaction history in the original session since
        // the history should be kept until there is a retryable write or transaction with a higher
        // txnNumber.
        retryableWriteTxnParticipantCatalog.reset();
    }

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).txnState.transitionTo(TransactionState::kInProgress);
        // Start tracking various transactions metrics.
        //
        // We measure the start time in both microsecond and millisecond resolution. The TickSource
        // provides microsecond resolution to record the duration of the transaction. The start
        // "wall clock" time can be considered an approximation to the microsecond measurement.
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
}

void TransactionParticipant::Participant::beginOrContinue(
    OperationContext* opCtx,
    TxnNumberAndRetryCounter txnNumberAndRetryCounter,
    boost::optional<bool> autocommit,
    boost::optional<bool> startTransaction) {
    if (_isInternalSessionForRetryableWrite()) {
        auto parentTxnParticipant =
            TransactionParticipant::get(opCtx, _session()->getParentSession());
        parentTxnParticipant.beginOrContinue(
            opCtx, {*_sessionId().getTxnNumber(), boost::none}, boost::none, boost::none);
    }

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

    if (txnNumberAndRetryCounter.getTxnNumber() <
        o().activeTxnNumberAndRetryCounter.getTxnNumber()) {
        const std::string currOperation =
            o().txnState.isInRetryableWriteMode() ? "retryable write" : "transaction";
        if (!autocommit) {
            uasserted(ErrorCodes::TransactionTooOld,
                      str::stream()
                          << "Retryable write with txnNumber "
                          << txnNumberAndRetryCounter.getTxnNumber() << " is prohibited on session "
                          << _sessionId() << " because a newer " << currOperation
                          << " with txnNumber " << o().activeTxnNumberAndRetryCounter.getTxnNumber()
                          << " has already started on this session.");
        } else {
            uasserted(ErrorCodes::TransactionTooOld,
                      str::stream()
                          << "Cannot start transaction with " << txnNumberAndRetryCounter.toBSON()
                          << " on session " << _sessionId() << " because a newer " << currOperation
                          << " with txnNumberAndRetryCounter "
                          << o().activeTxnNumberAndRetryCounter.toBSON()
                          << " has already started on this session.");
        }
    }

    // Requests without an autocommit field are interpreted as retryable writes. They cannot specify
    // startTransaction, which is verified earlier when parsing the request.
    if (!autocommit) {
        invariant(!startTransaction);
        invariant(!txnNumberAndRetryCounter.getTxnRetryCounter(),
                  "Cannot specify a txnRetryCounter for retryable write");
        _beginOrContinueRetryableWrite(opCtx, txnNumberAndRetryCounter);
        return;
    }

    // Attempt to continue a multi-statement transaction. In this case, it is required that
    // autocommit be given as an argument on the request, and currently it can only be false, which
    // is verified earlier when parsing the request.
    invariant(*autocommit == false);
    invariant(opCtx->inMultiDocumentTransaction());
    if (txnNumberAndRetryCounter.getTxnRetryCounter()) {
        uassert(ErrorCodes::InvalidOptions,
                "txnRetryCounter is only supported in sharded clusters",
                serverGlobalParams.clusterRole != ClusterRole::None);
        invariant(*txnNumberAndRetryCounter.getTxnRetryCounter() >= 0,
                  "Cannot specify a negative txnRetryCounter");
    } else {
        txnNumberAndRetryCounter.setTxnRetryCounter(0);
    }

    if (!startTransaction) {
        _continueMultiDocumentTransaction(opCtx, txnNumberAndRetryCounter);
        return;
    }

    // Attempt to start a multi-statement transaction, which requires startTransaction be given as
    // an argument on the request. The 'startTransaction' argument currently can only be specified
    // as true, which is verified earlier, when parsing the request.
    invariant(*startTransaction);

    auto isRetry = _verifyCanBeginMultiDocumentTransaction(opCtx, txnNumberAndRetryCounter);
    if (isRetry) {
        // This is a retry for the active transaction, so we don't throw, and we also don't need to
        // start the transaction since that already happened.
        return;
    }
    _beginMultiDocumentTransaction(opCtx, txnNumberAndRetryCounter);
}

void TransactionParticipant::Participant::beginOrContinueTransactionUnconditionally(
    OperationContext* opCtx, TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
    invariant(opCtx->inMultiDocumentTransaction());

    // We don't check or fetch any on-disk state, so treat the transaction as 'valid' for the
    // purposes of this method and continue the transaction unconditionally
    p().isValid = true;

    if (o().activeTxnNumberAndRetryCounter.getTxnNumber() !=
        txnNumberAndRetryCounter.getTxnNumber()) {
        if (!txnNumberAndRetryCounter.getTxnRetryCounter()) {
            txnNumberAndRetryCounter.setTxnRetryCounter(0);
        }
        _beginMultiDocumentTransaction(opCtx, txnNumberAndRetryCounter);
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

SharedSemiFuture<void> TransactionParticipant::Participant::onCompletion() const {
    if (!o().txnState._completionPromise) {
        // The participant is not in progress or in prepare.
        invariant(!o().txnState.isOpen());
        return Future<void>::makeReady();
    }

    // The participant is in progress or in prepare, so return a future that will be signaled when
    // the participant commits or aborts.
    invariant(o().txnState.isOpen());
    return o().txnState._completionPromise->getFuture();
}

SharedSemiFuture<void>
TransactionParticipant::Participant::onConflictingInternalTransactionCompletion(
    OperationContext* opCtx) const {
    auto& retryableWriteTxnParticipantCatalog =
        getRetryableWriteTransactionParticipantCatalog(opCtx);
    invariant(retryableWriteTxnParticipantCatalog.isValid());

    for (const auto& [_, txnParticipant] : retryableWriteTxnParticipantCatalog.getParticipants()) {
        if (txnParticipant._sessionId() == _sessionId() ||
            !txnParticipant._isInternalSessionForRetryableWrite()) {
            continue;
        }
        if (txnParticipant.transactionIsOpen()) {
            return txnParticipant.onCompletion();
        }
    }

    // There is no conflicting internal transaction.
    return Future<void>::makeReady();
}

void TransactionParticipant::Participant::_setReadSnapshot(OperationContext* opCtx,
                                                           repl::ReadConcernArgs readConcernArgs) {
    if (readConcernArgs.getArgsAtClusterTime()) {
        // Read concern code should have already set the timestamp on the recovery unit.
        const auto readTimestamp = readConcernArgs.getArgsAtClusterTime()->asTimestamp();
        const auto ruTs = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
        invariant(readTimestamp == ruTs,
                  "readTimestamp: {}, pointInTime: {}"_format(readTimestamp.toString(),
                                                              ruTs ? ruTs->toString() : "none"));

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).transactionMetricsObserver.onChooseReadTimestamp(readTimestamp);
    } else if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
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
    auto oplogInfo = LocalOplogInfo::get(opCtx);
    _oplogSlots = oplogInfo->getNextOpTimes(opCtx, numSlotsToReserve);

    // Release the WUOW state since this WUOW is no longer in use.
    wuow.release();

    // We must lock the Client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    // Save the RecoveryUnit from the new transaction and replace it with an empty one.
    _recoveryUnit = opCtx->releaseAndReplaceRecoveryUnit();

    // End two-phase locking on locker manually since the WUOW has been released.
    _opCtx->lockState()->endWriteUnitOfWork();
}

TransactionParticipant::OplogSlotReserver::~OplogSlotReserver() {
    if (MONGO_unlikely(hangBeforeReleasingTransactionOplogHole.shouldFail())) {
        LOGV2(22520,
              "transaction - hangBeforeReleasingTransactionOplogHole fail point enabled. Blocking "
              "until fail point is disabled");
        hangBeforeReleasingTransactionOplogHole.pauseWhileSet();
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

    _locker = opCtx->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()), wl);
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

    _recoveryUnit = opCtx->releaseAndReplaceRecoveryUnit();

    _apiParameters = APIParameters::get(opCtx);
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
    ScopeGuard onError([&] {
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

    if (MONGO_unlikely(restoreLocksFail.shouldFail())) {
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
    opCtx->swapLockState(std::move(_locker), lk);
    opCtx->lockState()->updateThreadIdToCurrentThread();

    auto oldState = opCtx->setRecoveryUnit(std::move(_recoveryUnit),
                                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    invariant(oldState == WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
              str::stream() << "RecoveryUnit state was " << oldState);

    opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(opCtx, _ruState));

    auto& apiParameters = APIParameters::get(opCtx);
    apiParameters = _apiParameters;

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
    _recoveryUnit = opCtx->releaseAndReplaceRecoveryUnit();
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

    invariant(o().activeTxnNumberAndRetryCounter.getTxnNumber() == opCtx->getTxnNumber());

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    {
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        o(lk).transactionMetricsObserver.onStash(ServerTransactionsMetrics::get(opCtx), tickSource);
        o(lk).transactionMetricsObserver.onTransactionOperation(
            opCtx, CurOp::get(opCtx)->debug().additiveMetrics, o().txnState.isPrepared());
    }

    invariant(!o().txnResourceStash);
    // If this is a prepared transaction, invariant that it does not hold the RSTL lock.
    invariant(!o().txnState.isPrepared() || !opCtx->lockState()->isRSTLLocked());
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

    ScopeGuard releaseOnError([&] {
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

    // Verify that transaction number and mode are as expected.
    if (opCtx->inMultiDocumentTransaction()) {
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "Attempted to run '" << cmdName << "' inside a transaction with "
                              << "session id" << _sessionId() << " and transaction number "
                              << *opCtx->getTxnNumber()
                              << " but the active transaction number on the session is "
                              << o().activeTxnNumberAndRetryCounter.getTxnNumber(),
                *opCtx->getTxnNumber() == o().activeTxnNumberAndRetryCounter.getTxnNumber());

        uassert(6611000,
                str::stream() << "Attempted to use the active transaction number "
                              << o().activeTxnNumberAndRetryCounter.getTxnNumber() << " in session "
                              << _sessionId()
                              << " to run a transaction but it corresponds to a retryable write",
                !o().txnState.isInRetryableWriteMode());
    } else {
        uassert(6564100,
                str::stream() << "Attempted to run '" << cmdName << "' as a retryable write with "
                              << "session id" << _sessionId() << " and transaction number "
                              << *opCtx->getTxnNumber()
                              << " but the active transaction number on the session is "
                              << o().activeTxnNumberAndRetryCounter.getTxnNumber(),
                *opCtx->getTxnNumber() == o().activeTxnNumberAndRetryCounter.getTxnNumber());
        uassert(6611001,
                str::stream() << "Attempted to use the active transaction number "
                              << o().activeTxnNumberAndRetryCounter.getTxnNumber() << " in session "
                              << _sessionId()
                              << " to run a retryable write but it corresponds to a transaction",
                o().txnState.isInRetryableWriteMode());
    }

    // If this is not a multi-document transaction, there is nothing to unstash.
    if (o().txnState.isInRetryableWriteMode()) {
        invariant(!o().txnResourceStash);
        return;
    }

    _checkIsCommandValidWithTxnState({*opCtx->getTxnNumber()}, cmdName);
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
    Lock::GlobalLock globalLock(opCtx, MODE_IX);

    // This begins the storage transaction and so we do it after acquiring the global lock.
    _setReadSnapshot(opCtx, repl::ReadConcernArgs::get(opCtx));

    // The Client lock must not be held when executing this failpoint as it will block currentOp
    // execution.
    if (MONGO_unlikely(hangAfterPreallocateSnapshot.shouldFail())) {
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

    ScopeGuard abortGuard([&] {
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
            LOGV2_FATAL_CONTINUE(22525,
                                 "Caught exception during abort of prepared transaction "
                                 "{txnNumber} on {lsid}: {error}",
                                 "Caught exception during abort of prepared transaction",
                                 "txnNumber"_attr = opCtx->getTxnNumber(),
                                 "lsid"_attr = _sessionId().toBSON(),
                                 "error"_attr = exceptionToStatus());
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
        if (transactionOp.getOpType() == repl::OpTypeEnum::kNoop) {
            // No-ops can't modify data, so there's no need to check if they involved a temporary
            // collection.
            continue;
        }

        transactionOperationUuids.insert(transactionOp.getUuid().value());
    }
    auto catalog = CollectionCatalog::get(opCtx);
    for (const auto& uuid : transactionOperationUuids) {
        auto collection = catalog->lookupCollectionByUUID(opCtx, uuid);
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "prepareTransaction failed because one of the transaction "
                                 "operations was done against a temporary collection '"
                              << collection->ns() << "'.",
                !collection->isTemporary());
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
        // Even if the prepared transaction contained no statements, we always reserve at least
        // 1 oplog slot for the prepare oplog entry.
        auto numSlotsToReserve = retrieveCompletedTransactionOperations(opCtx).size();
        numSlotsToReserve += p().numberOfPrePostImagesToWrite;
        oplogSlotReserver.emplace(opCtx, std::max(1, static_cast<int>(numSlotsToReserve)));
        invariant(oplogSlotReserver->getSlots().size() >= 1);
        prepareOplogSlot = oplogSlotReserver->getLastSlot();
        reservedSlots = oplogSlotReserver->getSlots();
        invariant(o().prepareOpTime.isNull(),
                  str::stream() << "This transaction has already reserved a prepareOpTime at: "
                                << o().prepareOpTime.toString());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).prepareOpTime = prepareOplogSlot;
        }

        if (MONGO_unlikely(hangAfterReservingPrepareTimestamp.shouldFail())) {
            // This log output is used in js tests so please leave it.
            LOGV2(22521,
                  "transaction - hangAfterReservingPrepareTimestamp fail point "
                  "enabled. Blocking until fail point is disabled. Prepare OpTime: "
                  "{prepareOpTime}",
                  "prepareOpTime"_attr = prepareOplogSlot);
            hangAfterReservingPrepareTimestamp.pauseWhileSet();
        }
    }
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    const auto wallClockTime = opCtx->getServiceContext()->getFastClockSource()->now();
    auto applyOpsOplogSlotAndOperationAssignment =
        opObserver->preTransactionPrepare(opCtx,
                                          reservedSlots,
                                          p().numberOfPrePostImagesToWrite,
                                          wallClockTime,
                                          &completedTransactionOperations);

    opCtx->recoveryUnit()->setPrepareTimestamp(prepareOplogSlot.getTimestamp());
    opCtx->getWriteUnitOfWork()->prepare();
    p().needToWriteAbortEntry = true;
    opObserver->onTransactionPrepare(opCtx,
                                     reservedSlots,
                                     &completedTransactionOperations,
                                     applyOpsOplogSlotAndOperationAssignment.get(),
                                     p().numberOfPrePostImagesToWrite,
                                     wallClockTime);

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

    if (MONGO_unlikely(hangAfterSettingPrepareStartTime.shouldFail())) {
        LOGV2(22522,
              "transaction - hangAfterSettingPrepareStartTime fail point enabled. Blocking "
              "until fail point is disabled");
        hangAfterSettingPrepareStartTime.pauseWhileSet();
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

repl::OpTime TransactionParticipant::Participant::getPrepareOpTimeForRecovery() const {
    return o().recoveryPrepareOpTime;
}

void TransactionParticipant::Participant::addTransactionOperation(
    OperationContext* opCtx, const repl::ReplOperation& operation) {
    // Ensure that we only ever add operations to an in progress transaction.
    if (!o().txnState.isInProgress() && _isInternalSessionForRetryableWrite()) {
        // Throw a uassert error instead of an invariant error if this is a retryable internal
        // transaction since all write statements are allowed to bypass the checks in
        // beginOrContinue if the transaction has already committed.
        uasserted(5875606,
                  "Cannot perform writes in a retryable internal transaction that has already "
                  "committed, aborted or prepared");
    }
    invariant(o().txnState.isInProgress(), str::stream() << "Current state: " << o().txnState);

    invariant(p().autoCommit && !*p().autoCommit &&
              o().activeTxnNumberAndRetryCounter.getTxnNumber() != kUninitializedTxnNumber);
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    const auto stmtIds = operation.getStatementIds();
    for (auto stmtId : stmtIds) {
        auto [_, inserted] = p().transactionStmtIds.insert(stmtId);
        uassert(5875600,
                str::stream() << "Found two operations using the same stmtId of " << stmtId,
                inserted);
    }
    p().transactionOperations.push_back(operation);

    p().transactionOperationBytes +=
        repl::DurableOplogEntry::getDurableReplOperationSize(operation);
    if (!operation.getPreImage().isEmpty()) {
        p().transactionOperationBytes += operation.getPreImage().objsize();
        if (operation.isChangeStreamPreImageRecordedInOplog() ||
            operation.isPreImageRecordedForRetryableInternalTransaction()) {
            ++p().numberOfPrePostImagesToWrite;
        }
    }
    if (!operation.getPostImage().isEmpty()) {
        p().transactionOperationBytes += operation.getPostImage().objsize();
        ++p().numberOfPrePostImagesToWrite;
    }

    auto transactionSizeLimitBytes = gTransactionSizeLimitBytes.load();
    uassert(ErrorCodes::TransactionTooLarge,
            str::stream() << "Total size of all transaction operations must be less than "
                          << "server parameter 'transactionSizeLimitBytes' = "
                          << transactionSizeLimitBytes,
            p().transactionOperationBytes <= static_cast<size_t>(transactionSizeLimitBytes));
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
    p().transactionStmtIds.clear();
    p().numberOfPrePostImagesToWrite = 0;
}

void TransactionParticipant::Participant::commitUnpreparedTransaction(OperationContext* opCtx) {
    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction must provide commitTimestamp to prepared transaction.",
            !o().txnState.isPrepared());

    auto& txnOps = retrieveCompletedTransactionOperations(opCtx);
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);

    opObserver->onUnpreparedTransactionCommit(opCtx, &txnOps, p().numberOfPrePostImagesToWrite);

    // Read-only transactions with all read concerns must wait for any data they read to be majority
    // committed. For local read concern this is to match majority read concern. For both local and
    // majority read concerns we do an untimestamped read, so we have no read timestamp to wait on.
    // Instead, we write a noop which is guaranteed to have a greater OpTime than any writes we
    // read.
    //
    // TODO (SERVER-41165): Snapshot read concern should wait on the read timestamp instead.
    auto wc = opCtx->getWriteConcern();
    auto needsNoopWrite = txnOps.empty() && !opCtx->getWriteConcern().usedDefaultConstructedWC;

    const size_t operationCount = p().transactionOperations.size();
    const size_t oplogOperationBytes = p().transactionOperationBytes;
    clearOperationsInMemory(opCtx);

    // _commitStorageTransaction can throw, but it is safe for the exception to be bubbled up to
    // the caller, since the transaction can still be safely aborted at this point.
    _commitStorageTransaction(opCtx);

    _finishCommitTransaction(opCtx, operationCount, oplogOperationBytes);

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
    ScopeGuard unlockGuard([&] { invariant(opCtx->lockState()->unlockRSTLforPrepare()); });

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
                    MONGO_unlikely(skipCommitTxnCheckPrepareMajorityCommitted.shouldFail()));
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

        if (opCtx->writesAreReplicated()) {
            invariant(!commitOplogEntryOpTime);
            // When this receiving node is not in a readable state, the cluster time gossiping
            // protocol is not enabled, thus it is necessary to advance it explicitely,
            // so that causal consistency is maintained in these situations.
            VectorClockMutable::get(opCtx)->tickClusterTimeTo(LogicalTime(commitTimestamp));

            // On primary, we reserve an oplog slot before committing the transaction so that no
            // writes that are causally related to the transaction commit enter the oplog at a
            // timestamp earlier than the commit oplog entry.
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
        LOGV2_FATAL_CONTINUE(22526,
                             "Caught exception during commit of prepared transaction {txnNumber} "
                             "on {lsid}: {error}",
                             "Caught exception during commit of prepared transaction",
                             "txnNumber"_attr = opCtx->getTxnNumber(),
                             "lsid"_attr = _sessionId().toBSON(),
                             "error"_attr = exceptionToStatus());
        std::terminate();
    }
}

void TransactionParticipant::Participant::_commitStorageTransaction(OperationContext* opCtx) {
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
}

void TransactionParticipant::Participant::_finishCommitTransaction(
    OperationContext* opCtx, size_t operationCount, size_t oplogOperationBytes) noexcept {
    {
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).txnState.transitionTo(TransactionState::kCommitted);

        o(lk).transactionMetricsObserver.onCommit(opCtx,
                                                  ServerTransactionsMetrics::get(opCtx),
                                                  tickSource,
                                                  &Top::get(opCtx->getServiceContext()),
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

APIParameters TransactionParticipant::Participant::getAPIParameters(OperationContext* opCtx) const {
    // If we have are in a retryable write, use the API parameters that the client passed in with
    // the write, instead of the first write's API parameters.
    if (o().txnResourceStash && !o().txnState.isInRetryableWriteMode()) {
        return o().txnResourceStash->getAPIParameters();
    }
    return APIParameters::get(opCtx);
}

void TransactionParticipant::Participant::setLastWriteOpTime(OperationContext* opCtx,
                                                             const repl::OpTime& lastWriteOpTime) {
    stdx::lock_guard<Client> lg(*opCtx->getClient());
    auto& curLastWriteOpTime = o(lg).lastWriteOpTime;
    invariant(lastWriteOpTime.isNull() || lastWriteOpTime > curLastWriteOpTime);
    curLastWriteOpTime = lastWriteOpTime;
}

bool TransactionParticipant::Observer::expiredAsOf(Date_t when) const {
    return o().txnState.isInProgress() && o().transactionExpireDate &&
        o().transactionExpireDate < when;
}

void TransactionParticipant::Participant::abortTransaction(OperationContext* opCtx) {
    if (_isInternalSessionForRetryableWrite() && o().txnState.isCommitted()) {
        // An error occurred while retrying an committed retryable internal transaction should
        // not modify the state of the committed transaction.
        return;
    }
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
    // TODO SERVER-58243: evaluate whether this is safe or whether acquiring the lock can block.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());

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
            LOGV2_FATAL_CONTINUE(
                22527,
                "Caught exception during abort of transaction that must write abort oplog "
                "entry {txnNumber} on {lsid}: {error}",
                "Caught exception during abort of transaction that must write abort oplog "
                "entry",
                "txnNumber"_attr = opCtx->getTxnNumber(),
                "lsid"_attr = _sessionId().toBSON(),
                "error"_attr = exceptionToStatus());
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
        invariant(opCtx->getTxnNumber() == o().activeTxnNumberAndRetryCounter.getTxnNumber());
        _abortTransactionOnSession(opCtx);
    } else if (opCtx->getTxnNumber() == o().activeTxnNumberAndRetryCounter.getTxnNumber()) {
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
                            o().txnResourceStash->getAPIParameters(),
                            o().txnResourceStash->getReadConcernArgs());
    }

    const auto nextState = o().txnState.isPrepared() ? TransactionState::kAbortedWithPrepare
                                                     : TransactionState::kAbortedWithoutPrepare;

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    if (o().txnResourceStash && opCtx->recoveryUnit()->getNoEvictionAfterRollback()) {
        o(lk).txnResourceStash->setNoEvictionAfterRollback();
    }
    _resetTransactionStateAndUnlock(&lk, nextState);

    _resetRetryableWriteState();
}

void TransactionParticipant::Participant::_cleanUpTxnResourceOnOpCtx(
    OperationContext* opCtx, TerminationCause terminationCause) {
    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(
        opCtx,
        &(opCtx->lockState()->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase()))->stats,
        terminationCause,
        APIParameters::get(opCtx),
        repl::ReadConcernArgs::get(opCtx));

    // Reset the WUOW. We should be able to abort empty transactions that don't have WUOW.
    if (opCtx->getWriteUnitOfWork()) {
        // We could have failed trying to get the initial global lock; in that case we will have a
        // WriteUnitOfWork but not have allocated the storage transaction.  That is the only case
        // where it is legal to abort a unit of work without the RSTL.
        invariant(opCtx->lockState()->isRSTLLocked() || !opCtx->recoveryUnit()->isActive());
        opCtx->setWriteUnitOfWork(nullptr);
    }

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
    invariant(UncommittedCatalogUpdates::get(opCtx).isEmpty());
}

void TransactionParticipant::Participant::_checkIsCommandValidWithTxnState(
    const TxnNumberAndRetryCounter& requestTxnNumberAndRetryCounter,
    const std::string& cmdName) const {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction with " << requestTxnNumberAndRetryCounter.toBSON()
                          << " has been aborted.",
            !o().txnState.isAborted());

    // Cannot change committed transaction but allow retrying:
    // - commitTransaction command.
    // - any command if the transaction is an internal transaction for retryable writes.
    uassert(ErrorCodes::TransactionCommitted,
            str::stream() << "Transaction with " << requestTxnNumberAndRetryCounter.toBSON()
                          << " has been committed.",
            !o().txnState.isCommitted() || cmdName == "commitTransaction" ||
                _isInternalSessionForRetryableWrite());

    // Disallow operations other than abort, prepare or commit on a prepared transaction
    uassert(ErrorCodes::PreparedTransactionInProgress,
            str::stream() << "Cannot call any operation other than abort, prepare or commit on"
                          << " a prepared transaction",
            !o().txnState.isPrepared() ||
                preparedTxnCmdAllowlist.find(cmdName) != preparedTxnCmdAllowlist.cend());
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
            invariant(o().activeTxnNumberAndRetryCounter.getTxnNumber() != kUninitializedTxnNumber);
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

    // If we are transitioning out of prepare, fulfill and reset the exit prepare promise.
    if (isPrepared()) {
        invariant(_exitPreparePromise);
        _exitPreparePromise->emplaceValue();
        _exitPreparePromise.reset();
    }

    _state = newState;

    // If we have transitioned into prepare, initialize the exit prepare promise so other threads
    // can wait for the participant to transition out of prepare.
    if (isPrepared()) {
        invariant(!_exitPreparePromise);
        _exitPreparePromise.emplace();
    }

    if (isOpen() && !_completionPromise) {
        // If we have transitioned into the in progress or prepare state, initialize the commit or
        // abort promise so other threads can wait for the participant to commit or abort.
        _completionPromise.emplace();
    } else if (!isOpen() && _completionPromise) {
        // If we have transitioned into the commited or aborted or none state, fulfill and reset the
        // commit or abort promise. If the state transition is caused by a refresh, the promise is
        // expected to have not been initialized and no work is required.
        _completionPromise->emplaceValue();
        _completionPromise.reset();
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
    APIParameters apiParameters,
    repl::ReadConcernArgs readConcernArgs) const {
    invariant(lockStats);

    StringBuilder s;

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", o().activeTxnNumberAndRetryCounter.getTxnNumber());
    parametersBuilder.append("txnRetryCounter",
                             *o().activeTxnNumberAndRetryCounter.getTxnRetryCounter());
    parametersBuilder.append("autocommit", p().autoCommit ? *p().autoCommit : true);
    apiParameters.appendInfo(&parametersBuilder);
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


void TransactionParticipant::Participant::_transactionInfoForLog(
    OperationContext* opCtx,
    const SingleThreadedLockStats* lockStats,
    TerminationCause terminationCause,
    APIParameters apiParameters,
    repl::ReadConcernArgs readConcernArgs,
    logv2::DynamicAttributes* pAttrs) const {
    invariant(lockStats);

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", o().activeTxnNumberAndRetryCounter.getTxnNumber());
    parametersBuilder.append("txnRetryCounter",
                             *o().activeTxnNumberAndRetryCounter.getTxnRetryCounter());
    parametersBuilder.append("autocommit", p().autoCommit ? *p().autoCommit : true);
    apiParameters.appendInfo(&parametersBuilder);
    readConcernArgs.appendInfo(&parametersBuilder);

    pAttrs->add("parameters", parametersBuilder.obj());

    const auto& singleTransactionStats = o().transactionMetricsObserver.getSingleTransactionStats();

    pAttrs->addDeepCopy("readTimestamp", singleTransactionStats.getReadTimestamp().toString());

    singleTransactionStats.getOpDebug()->additiveMetrics.report(pAttrs);

    StringData terminationCauseString =
        terminationCause == TerminationCause::kCommitted ? "committed" : "aborted";
    pAttrs->add("terminationCause", terminationCauseString);

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTick = tickSource->getTicks();

    pAttrs->add("timeActive", singleTransactionStats.getTimeActiveMicros(tickSource, curTick));
    pAttrs->add("timeInactive", singleTransactionStats.getTimeInactiveMicros(tickSource, curTick));

    // Number of yields is always 0 in multi-document transactions, but it is included mainly to
    // match the format with other slow operation logging messages.
    pAttrs->add("numYields", 0);
    // Aggregate lock statistics.

    BSONObjBuilder locks;
    lockStats->report(&locks);
    pAttrs->add("locks", locks.obj());

    if (singleTransactionStats.getOpDebug()->storageStats)
        pAttrs->add("storage", singleTransactionStats.getOpDebug()->storageStats->toBSON());

    // It is possible for a slow transaction to have aborted in the prepared state if an
    // exception was thrown before prepareTransaction succeeds.
    const auto totalPreparedDuration = durationCount<Microseconds>(
        singleTransactionStats.getPreparedDuration(tickSource, curTick));
    const bool txnWasPrepared = totalPreparedDuration > 0;
    pAttrs->add("wasPrepared", txnWasPrepared);
    if (txnWasPrepared) {
        pAttrs->add("totalPreparedDuration", Microseconds(totalPreparedDuration));
        pAttrs->add("prepareOpTime", o().prepareOpTime);
    }

    // Total duration of the transaction.
    pAttrs->add(
        "duration",
        duration_cast<Milliseconds>(singleTransactionStats.getDuration(tickSource, curTick)));
}

// Needs to be kept in sync with _transactionInfoForLog
BSONObj TransactionParticipant::Participant::_transactionInfoBSONForLog(
    OperationContext* opCtx,
    const SingleThreadedLockStats* lockStats,
    TerminationCause terminationCause,
    APIParameters apiParameters,
    repl::ReadConcernArgs readConcernArgs) const {
    invariant(lockStats);

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", o().activeTxnNumberAndRetryCounter.getTxnNumber());
    parametersBuilder.append("autocommit", p().autoCommit ? *p().autoCommit : true);
    apiParameters.appendInfo(&parametersBuilder);
    readConcernArgs.appendInfo(&parametersBuilder);

    BSONObjBuilder logLine;
    {
        BSONObjBuilder attrs = logLine.subobjStart("attr");
        attrs.append("parameters", parametersBuilder.obj());

        const auto& singleTransactionStats =
            o().transactionMetricsObserver.getSingleTransactionStats();

        attrs.append("readTimestamp", singleTransactionStats.getReadTimestamp().toString());

        attrs.appendElements(singleTransactionStats.getOpDebug()->additiveMetrics.reportBSON());

        StringData terminationCauseString =
            terminationCause == TerminationCause::kCommitted ? "committed" : "aborted";
        attrs.append("terminationCause", terminationCauseString);

        auto tickSource = opCtx->getServiceContext()->getTickSource();
        auto curTick = tickSource->getTicks();

        attrs.append("timeActiveMicros",
                     durationCount<Microseconds>(
                         singleTransactionStats.getTimeActiveMicros(tickSource, curTick)));
        attrs.append("timeInactiveMicros",
                     durationCount<Microseconds>(
                         singleTransactionStats.getTimeInactiveMicros(tickSource, curTick)));

        // Number of yields is always 0 in multi-document transactions, but it is included mainly to
        // match the format with other slow operation logging messages.
        attrs.append("numYields", 0);
        // Aggregate lock statistics.

        BSONObjBuilder locks;
        lockStats->report(&locks);
        attrs.append("locks", locks.obj());

        if (singleTransactionStats.getOpDebug()->storageStats)
            attrs.append("storage", singleTransactionStats.getOpDebug()->storageStats->toBSON());

        // It is possible for a slow transaction to have aborted in the prepared state if an
        // exception was thrown before prepareTransaction succeeds.
        const auto totalPreparedDuration = durationCount<Microseconds>(
            singleTransactionStats.getPreparedDuration(tickSource, curTick));
        const bool txnWasPrepared = totalPreparedDuration > 0;
        attrs.append("wasPrepared", txnWasPrepared);
        if (txnWasPrepared) {
            attrs.append("totalPreparedDurationMicros", totalPreparedDuration);
            attrs.append("prepareOpTime", o().prepareOpTime.toBSON());
        }

        // Total duration of the transaction.
        attrs.append(
            "durationMillis",
            duration_cast<Milliseconds>(singleTransactionStats.getDuration(tickSource, curTick))
                .count());
    }
    return logLine.obj();
}

void TransactionParticipant::Participant::_logSlowTransaction(
    OperationContext* opCtx,
    const SingleThreadedLockStats* lockStats,
    TerminationCause terminationCause,
    APIParameters apiParameters,
    repl::ReadConcernArgs readConcernArgs) {
    // Only log multi-document transactions.
    if (!o().txnState.isInRetryableWriteMode()) {
        const auto tickSource = opCtx->getServiceContext()->getTickSource();
        const auto opDuration = duration_cast<Milliseconds>(
            o().transactionMetricsObserver.getSingleTransactionStats().getDuration(
                tickSource, tickSource->getTicks()));

        if (shouldLogSlowOpWithSampling(opCtx,
                                        logv2::LogComponent::kTransaction,
                                        opDuration,
                                        Milliseconds(serverGlobalParams.slowMS))
                .first) {
            logv2::DynamicAttributes attr;
            _transactionInfoForLog(
                opCtx, lockStats, terminationCause, apiParameters, readConcernArgs, &attr);
            LOGV2_OPTIONS(51802, {logv2::LogComponent::kTransaction}, "transaction", attr);
        }
    }
}

void TransactionParticipant::Participant::_setNewTxnNumberAndRetryCounter(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    uassert(ErrorCodes::PreparedTransactionInProgress,
            "Cannot change transaction number while the session has a prepared transaction",
            !o().txnState.isInSet(TransactionState::kPrepared));

    LOGV2_FOR_TRANSACTION(
        23984,
        4,
        "New transaction started with txnNumber: {txnNumberAndRetryCounter} on session with lsid "
        "{lsid}",
        "New transaction started",
        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
        "lsid"_attr = _sessionId(),
        "apiParameters"_attr = APIParameters::get(opCtx).toBSON());

    // Abort the existing transaction if it's not prepared, committed, or aborted.
    if (o().txnState.isInProgress()) {
        _abortTransactionOnSession(opCtx);
    }
    // If txnNumber ordering applies, abort any child transactions with a lesser txnNumber.
    auto clientTxnNumber = getClientTxnNumber(txnNumberAndRetryCounter);
    if (clientTxnNumber.has_value()) {
        getRetryableWriteTransactionParticipantCatalog(opCtx).abortSupersededTransactions(
            opCtx, *clientTxnNumber);
    }

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    o(lk).activeTxnNumberAndRetryCounter = txnNumberAndRetryCounter;
    o(lk).lastWriteOpTime = repl::OpTime();

    // Reset the retryable writes state
    _resetRetryableWriteState();

    // Reset the transactions metrics
    o(lk).transactionMetricsObserver.resetSingleTransactionStats(txnNumberAndRetryCounter);

    // Reset the transactional state
    _resetTransactionStateAndUnlock(&lk, TransactionState::kNone);

    invariant(!lk);
    if (isParentSessionId(_sessionId())) {
        // Only observe parent sessions because retryable transactions begin the same txnNumber on
        // their parent session.
        OperationContextSession::observeNewTxnNumberStarted(
            opCtx, _sessionId(), txnNumberAndRetryCounter.getTxnNumber());
    }
}

void RetryableWriteTransactionParticipantCatalog::addParticipant(
    const TransactionParticipant::Participant& participant) {
    invariant(participant.p().isValid);

    const auto txnNumber = participant._activeRetryableWriteTxnNumber();
    invariant(*txnNumber >= _activeTxnNumber);

    if (txnNumber > _activeTxnNumber) {
        reset();
        _activeTxnNumber = *txnNumber;
    }
    if (auto it = _participants.find(participant._sessionId()); it != _participants.end()) {
        invariant(it->second._tp == participant._tp);
    } else {
        _participants.emplace(participant._sessionId(), participant);
    }
}

void RetryableWriteTransactionParticipantCatalog::reset() {
    _activeTxnNumber = kUninitializedTxnNumber;
    _participants.clear();
    _hasSeenIncomingConflictingRetryableTransaction = false;
}

void RetryableWriteTransactionParticipantCatalog::markAsValid() {
    invariant(std::all_of(_participants.begin(), _participants.end(), [](const auto& it) {
        return it.second.p().isValid;
    }));
    _isValid = true;
}

void RetryableWriteTransactionParticipantCatalog::invalidate() {
    reset();
    _isValid = false;
}

bool RetryableWriteTransactionParticipantCatalog::isValid() const {
    return _isValid && std::all_of(_participants.begin(), _participants.end(), [](const auto& it) {
               return it.second.p().isValid;
           });
}

void RetryableWriteTransactionParticipantCatalog::checkForConflictingInternalTransactions(
    OperationContext* opCtx,
    TxnNumber incomingClientTxnNumber,
    const TxnNumberAndRetryCounter& incomingTxnNumberAndRetryCounter) {
    invariant(isValid());

    for (auto&& it : _participants) {
        auto& sessionId = it.first;
        auto& txnParticipant = it.second;

        if (sessionId == opCtx->getLogicalSessionId() ||
            !txnParticipant._isInternalSessionForRetryableWrite()) {
            continue;
        }

        if (!txnParticipant.transactionIsOpen()) {
            // The transaction isn't open, so it can't conflict with an incoming transaction.
            continue;
        }

        auto clientTxnNumber =
            txnParticipant.getClientTxnNumber(txnParticipant.getActiveTxnNumberAndRetryCounter());
        invariant(clientTxnNumber.has_value());
        if (*clientTxnNumber < incomingClientTxnNumber) {
            // To match the behavior of client transactions when a logically earlier prepared
            // transaction is in progress, throw an error to block the new transaction until the
            // earlier one exists prepare.
            uassert(ErrorCodes::RetryableTransactionInProgress,
                    "Operation conflicts with an earlier retryable transaction in prepare",
                    !txnParticipant.transactionIsPrepared());

            // Otherwise skip this transaction because it will be aborted when this one begins.
            continue;
        }

        if (!_hasSeenIncomingConflictingRetryableTransaction &&
            txnParticipant.transactionIsInProgress()) {
            // Only abort when the transaction is in progress since other states may not be safe,
            // e.g. prepare.
            _hasSeenIncomingConflictingRetryableTransaction = true;
            txnParticipant._abortTransactionOnSession(opCtx);
        } else {
            uassert(
                ErrorCodes::RetryableTransactionInProgress,
                str::stream() << "Cannot run operation with session id "
                              << opCtx->getLogicalSessionId() << " and transaction number "
                              << incomingTxnNumberAndRetryCounter.getTxnNumber()
                              << " because it conflicts with an active operation with session id "
                              << sessionId << " and transaction number "
                              << txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber()
                              << " in state " << txnParticipant.o().txnState,
                !txnParticipant.transactionIsOpen());
        }
    }
}

void RetryableWriteTransactionParticipantCatalog::abortSupersededTransactions(
    OperationContext* opCtx, TxnNumber incomingClientTxnNumber) {
    if (!isValid()) {
        // This was called while refreshing from storage or applying ops on a secondary, so skip it.
        return;
    }

    for (auto&& it : _participants) {
        auto& sessionId = it.first;
        auto& txnParticipant = it.second;

        if (sessionId == opCtx->getLogicalSessionId() ||
            !txnParticipant._isInternalSessionForRetryableWrite()) {
            continue;
        }

        // We should never try to abort a prepared transaction. We should have earlier thrown either
        // RetryableTransactionInProgress or PreparedTransactionInProgress.
        invariant(!txnParticipant.transactionIsPrepared(),
                  str::stream() << "Transaction on session " << sessionId
                                << " unexpectedly in prepare");

        auto clientTxnNumber =
            txnParticipant.getClientTxnNumber(txnParticipant.getActiveTxnNumberAndRetryCounter());
        invariant(clientTxnNumber.has_value());
        if (*clientTxnNumber < incomingClientTxnNumber &&
            txnParticipant.transactionIsInProgress()) {
            txnParticipant._abortTransactionOnSession(opCtx);
        }
    }
}

void TransactionParticipant::Participant::refreshFromStorageIfNeeded(OperationContext* opCtx) {
    return _refreshFromStorageIfNeeded(opCtx, true);
}

void TransactionParticipant::Participant::refreshFromStorageIfNeededNoOplogEntryFetch(
    OperationContext* opCtx) {
    return _refreshFromStorageIfNeeded(opCtx, false);
}

void TransactionParticipant::Participant::_refreshFromStorageIfNeeded(OperationContext* opCtx,
                                                                      bool fetchOplogEntries) {
    _refreshSelfFromStorageIfNeeded(opCtx, fetchOplogEntries);
    if (!_isInternalSessionForNonRetryableWrite()) {
        // Internal sessions for non-retryable writes only support transactions and those
        // transactions are not retryable or related to the retryable write or transaction the
        // original sessions that those writes run in, so there is no need to do a cross-session
        // refresh.
        _refreshActiveTransactionParticipantsFromStorageIfNeeded(opCtx, fetchOplogEntries);
    }
}

void TransactionParticipant::Participant::_refreshSelfFromStorageIfNeeded(OperationContext* opCtx,
                                                                          bool fetchOplogEntries) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(!opCtx->lockState()->isLocked());

    if (p().isValid) {
        return;
    }

    auto activeTxnHistory = fetchActiveTransactionHistory(opCtx, _sessionId(), fetchOplogEntries);
    const auto& lastTxnRecord = activeTxnHistory.lastTxnRecord;
    if (lastTxnRecord) {
        stdx::lock_guard<Client> lg(*opCtx->getClient());
        o(lg).activeTxnNumberAndRetryCounter.setTxnNumber(lastTxnRecord->getTxnNum());
        o(lg).activeTxnNumberAndRetryCounter.setTxnRetryCounter([&] {
            if (lastTxnRecord->getState()) {
                if (lastTxnRecord->getTxnRetryCounter().has_value()) {
                    return *lastTxnRecord->getTxnRetryCounter();
                }
                return 0;
            }
            return kUninitializedTxnRetryCounter;
        }());
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
                // We should never be refreshing a prepared or in-progress transaction from
                // storage since it should already be in a valid state after replication
                // recovery.
                case DurableTxnStateEnum::kPrepared:
                case DurableTxnStateEnum::kInProgress:
                    MONGO_UNREACHABLE;
            }
        }
    }

    if (!_isInternalSession()) {
        const auto txnNumber = fetchHighestTxnNumberWithInternalSessions(opCtx, _sessionId());
        if (txnNumber > o().activeTxnNumberAndRetryCounter.getTxnNumber()) {
            _setNewTxnNumberAndRetryCounter(opCtx, {txnNumber, kUninitializedTxnRetryCounter});
        }
    }

    p().isValid = true;
}

void TransactionParticipant::Participant::_refreshActiveTransactionParticipantsFromStorageIfNeeded(
    OperationContext* opCtx, bool fetchOplogEntries) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(!opCtx->lockState()->isLocked());

    auto parentTxnParticipant = _isInternalSessionForRetryableWrite()
        ? TransactionParticipant::get(opCtx, _session()->getParentSession())
        : *this;
    parentTxnParticipant._refreshSelfFromStorageIfNeeded(opCtx, fetchOplogEntries);

    auto& retryableWriteTxnParticipantCatalog =
        getRetryableWriteTransactionParticipantCatalog(opCtx);

    if (retryableWriteTxnParticipantCatalog.isValid()) {
        return;
    }

    // Populate the catalog if the session is running a retryable write, and reset it otherwise.
    if (const auto activeRetryableWriteTxnNumber =
            parentTxnParticipant._activeRetryableWriteTxnNumber()) {
        // Add parent Participant.
        retryableWriteTxnParticipantCatalog.addParticipant(parentTxnParticipant);

        // Add child participants.
        std::vector<TransactionParticipant::Participant> childTxnParticipants;

        // Make sure that every child session has a corresponding
        // Session/TransactionParticipant.
        try {
            performReadWithNoTimestampDBDirectClient(opCtx, [&](DBDirectClient* client) {
                FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
                findRequest.setFilter(BSON(SessionTxnRecord::kParentSessionIdFieldName
                                           << parentTxnParticipant._sessionId().toBSON()
                                           << (SessionTxnRecord::kSessionIdFieldName + "." +
                                               LogicalSessionId::kTxnNumberFieldName)
                                           << BSON("$gte" << *activeRetryableWriteTxnNumber)));
                findRequest.setProjection(BSON(SessionTxnRecord::kSessionIdFieldName << 1));
                findRequest.setHint(
                    BSON("$hint" << MongoDSessionCatalog::kConfigTxnsPartialIndexName));

                auto cursor = client->find(findRequest);

                while (cursor->more()) {
                    const auto doc = cursor->next();
                    const auto childLsid = LogicalSessionId::parse(
                        IDLParserContext("LogicalSessionId"), doc.getObjectField("_id"));
                    uassert(6202001,
                            str::stream()
                                << "Refresh expected the highest transaction number in the session "
                                << parentTxnParticipant._sessionId() << " to be "
                                << *activeRetryableWriteTxnNumber << " found a "
                                << NamespaceString::kSessionTransactionsTableNamespace
                                << " entry for an internal transaction for retryable writes with "
                                << "transaction number " << *childLsid.getTxnNumber(),
                            *childLsid.getTxnNumber() == *activeRetryableWriteTxnNumber);
                    auto sessionCatalog = SessionCatalog::get(opCtx);
                    sessionCatalog->scanSession(
                        childLsid,
                        [&](const ObservableSession& osession) {
                            auto childTxnParticipant =
                                TransactionParticipant::get(opCtx, osession.get());
                            childTxnParticipants.push_back(childTxnParticipant);
                        },
                        SessionCatalog::ScanSessionCreateSession::kYes);
                }
            });
        } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
            rethrowPartialIndexQueryBadValueWithContext(ex);
            throw;
        }

        for (auto& childTxnParticipant : childTxnParticipants) {
            childTxnParticipant._refreshSelfFromStorageIfNeeded(opCtx, fetchOplogEntries);
            retryableWriteTxnParticipantCatalog.addParticipant(childTxnParticipant);
        }
    } else {
        retryableWriteTxnParticipantCatalog.reset();
    }

    retryableWriteTxnParticipantCatalog.markAsValid();
}

void TransactionParticipant::Participant::onWriteOpCompletedOnPrimary(
    OperationContext* opCtx,
    std::vector<StmtId> stmtIdsWritten,
    const SessionTxnRecord& sessionTxnRecord) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    invariant(sessionTxnRecord.getSessionId() == _sessionId());
    invariant(sessionTxnRecord.getTxnNum() == o().activeTxnNumberAndRetryCounter.getTxnNumber());

    if (o().txnState.isCommitted()) {
        // Only write statements in retryable internal transaction can bypass the checks in
        // beginOrContinue and get to here.
        invariant(_isInternalSessionForRetryableWrite());
        uasserted(5875603,
                  "Cannot perform writes in a retryable internal transaction that has already "
                  "committed");
    }

    // Sanity check that we don't double-execute statements
    for (const auto stmtId : stmtIdsWritten) {
        const auto stmtOpTime = _checkStatementExecutedSelf(stmtId);
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

    updateSessionEntry(opCtx, updateRequest, _sessionId(), sessionTxnRecord.getTxnNum());
    _registerUpdateCacheOnCommit(
        opCtx, std::move(stmtIdsWritten), sessionTxnRecord.getLastWriteOpTime());
}

void TransactionParticipant::Participant::onRetryableWriteCloningCompleted(
    OperationContext* opCtx,
    std::vector<StmtId> stmtIdsWritten,
    const SessionTxnRecord& sessionTxnRecord) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    invariant(sessionTxnRecord.getSessionId() == _sessionId());
    invariant(sessionTxnRecord.getTxnNum() == o().activeTxnNumberAndRetryCounter.getTxnNumber());

    const auto updateRequest = _makeUpdateRequest(sessionTxnRecord);

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest, _sessionId(), sessionTxnRecord.getTxnNum());
    _registerUpdateCacheOnCommit(
        opCtx, std::move(stmtIdsWritten), sessionTxnRecord.getLastWriteOpTime());
}

void TransactionParticipant::Participant::_invalidate(WithLock wl) {
    p().isValid = false;
    o(wl).activeTxnNumberAndRetryCounter = {kUninitializedTxnNumber, kUninitializedTxnRetryCounter};
    o(wl).lastWriteOpTime = repl::OpTime();

    // Reset the transactions metrics.
    o(wl).transactionMetricsObserver.resetSingleTransactionStats(
        o().activeTxnNumberAndRetryCounter);
}

void TransactionParticipant::Participant::_resetRetryableWriteState() {
    p().activeTxnCommittedStatements.clear();
    p().hasIncompleteHistory = false;
}

void TransactionParticipant::Participant::_resetTransactionStateAndUnlock(
    stdx::unique_lock<Client>* lk, TransactionState::StateFlag state) {
    invariant(lk && lk->owns_lock());

    // If we are transitioning to kNone, we are either starting a new transaction or aborting a
    // prepared transaction for rollback. In the latter case, we will need to relax the
    // invariant that prevents transitioning from kPrepared to kNone.
    if (o().txnState.isPrepared() && state == TransactionState::kNone) {
        o(*lk).txnState.transitionTo(
            state, TransactionState::TransitionValidation::kRelaxTransitionValidation);
    } else {
        o(*lk).txnState.transitionTo(state);
    }

    p().transactionOperationBytes = 0;
    p().transactionOperations.clear();
    p().transactionStmtIds.clear();
    o(*lk).prepareOpTime = repl::OpTime();
    o(*lk).recoveryPrepareOpTime = repl::OpTime();
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
    // Get the RetryableWriteTransactionParticipantCatalog without checking the opCtx has checked
    // out this session since by design it is illegal to invalidate sessions with an opCtx that has
    // a session checked out.
    auto& retryableWriteTxnParticipantCatalog =
        getRetryableWriteTransactionParticipantCatalog(_session());
    if (!_isInternalSessionForNonRetryableWrite()) {
        // Don't invalidate the RetryableWriteTransactionParticipantCatalog upon invalidating an
        // internal transaction for a non-retryable write since the transaction is unrelated to
        // the retryable write or transaction in the original session that the write runs in.
        retryableWriteTxnParticipantCatalog.invalidate();
    }

    _resetTransactionStateAndUnlock(&lk, TransactionState::kNone);
}

boost::optional<repl::OplogEntry> TransactionParticipant::Participant::checkStatementExecuted(
    OperationContext* opCtx, StmtId stmtId) const {
    const auto stmtOpTime = _checkStatementExecuted(opCtx, stmtId);

    if (!stmtOpTime) {
        return boost::none;
    }

    // Use a SideTransactionBlock since it is illegal to scan the oplog while in a write unit of
    // work.
    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);
    TransactionHistoryIterator txnIter(*stmtOpTime);
    while (txnIter.hasNext()) {
        const auto entry = txnIter.next(opCtx);

        if (entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps) {
            validateTransactionHistoryApplyOpsOplogEntry(entry);

            std::vector<repl::OplogEntry> innerEntries;
            repl::ApplyOps::extractOperationsTo(entry, entry.getEntry().toBSON(), &innerEntries);
            for (const auto& innerEntry : innerEntries) {
                auto stmtIds = innerEntry.getStatementIds();
                if (std::find(stmtIds.begin(), stmtIds.end(), stmtId) != stmtIds.end()) {
                    return innerEntry;
                }
            }
        } else {
            auto stmtIds = entry.getStatementIds();
            invariant(!stmtIds.empty());
            if (std::find(stmtIds.begin(), stmtIds.end(), stmtId) != stmtIds.end()) {
                return entry;
            }
        }
    }

    MONGO_UNREACHABLE;
}

bool TransactionParticipant::Participant::checkStatementExecutedNoOplogEntryFetch(
    OperationContext* opCtx, StmtId stmtId) const {
    return bool(_checkStatementExecuted(opCtx, stmtId));
}

boost::optional<repl::OpTime> TransactionParticipant::Participant::_checkStatementExecuted(
    OperationContext* opCtx, StmtId stmtId) const {
    const auto& retryableWriteTxnParticipantCatalog =
        getRetryableWriteTransactionParticipantCatalog(opCtx);
    invariant(retryableWriteTxnParticipantCatalog.isValid());
    invariant(retryableWriteTxnParticipantCatalog.getActiveTxnNumber() ==
              _activeRetryableWriteTxnNumber());

    if (auto opTime = _checkStatementExecutedSelf(stmtId)) {
        return opTime;
    }

    for (const auto& [sessionId, txnParticipant] :
         retryableWriteTxnParticipantCatalog.getParticipants()) {
        if (_sessionId() == sessionId || txnParticipant.transactionIsAborted()) {
            continue;
        }
        if (auto opTime = txnParticipant._checkStatementExecutedSelf(stmtId)) {
            return opTime;
        }
    }
    return boost::none;
}

boost::optional<repl::OpTime> TransactionParticipant::Participant::_checkStatementExecutedSelf(
    StmtId stmtId) const {
    invariant(p().isValid);
    if (_isInternalSessionForRetryableWrite()) {
        invariant(!transactionIsAborted());
    }

    const auto it = p().activeTxnCommittedStatements.find(stmtId);
    if (it == p().activeTxnCommittedStatements.end()) {
        uassert(ErrorCodes::IncompleteTransactionHistory,
                str::stream() << "Incomplete history detected for transaction "
                              << o().activeTxnNumberAndRetryCounter.getTxnNumber() << " on session "
                              << _sessionId(),
                !p().hasIncompleteHistory);

        return boost::none;
    }

    return it->second;
}

UpdateRequest TransactionParticipant::Participant::_makeUpdateRequest(
    const SessionTxnRecord& sessionTxnRecord) const {
    auto updateRequest = UpdateRequest();
    updateRequest.setNamespaceString(NamespaceString::kSessionTransactionsTableNamespace);

    updateRequest.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(sessionTxnRecord.toBSON()));
    updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName << _sessionId().toBSON()));
    updateRequest.setUpsert(true);

    return updateRequest;
}

void TransactionParticipant::Participant::addCommittedStmtIds(
    OperationContext* opCtx,
    const std::vector<StmtId>& stmtIdsCommitted,
    const repl::OpTime& writeOpTime) {
    stdx::lock_guard<Client> lg(*opCtx->getClient());
    for (auto stmtId : stmtIdsCommitted) {
        p().activeTxnCommittedStatements.emplace(stmtId, writeOpTime);
    }
}

void TransactionParticipant::Participant::handleWouldChangeOwningShardError(
    OperationContext* opCtx,
    std::shared_ptr<const WouldChangeOwningShardInfo> wouldChangeOwningShardInfo) {
    if (o().txnState.isNone() && p().autoCommit == boost::none) {
        // If this was a retryable write, reset the transaction state so this participant can be
        // reused for the transaction mongos will use to handle the WouldChangeOwningShard error.

        if (opCtx->getClient()->isInDirectClient()) {
            return;
        }

        invariant(opCtx->getTxnNumber());
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        _resetRetryableWriteState();
    } else if (_isInternalSessionForRetryableWrite()) {
        // If this was a retryable transaction, add a sentinel noop to the transaction's operations
        // so retries can detect that a WouldChangeOwningShard error was thrown and know to throw
        // IncompleteTransactionHistory.

        uassert(5918601,
                "Expected retryable internal session to have a transaction, not a retryable write",
                p().autoCommit != boost::none);
        repl::ReplOperation operation;
        operation.setOpType(repl::OpTypeEnum::kNoop);
        operation.setObject(kWouldChangeOwningShardSentinel);
        // Set the "o2" field to differentiate between a WouldChangeOwningShard noop oplog entry
        // written while handling a WouldChangeOwningShard error and a noop oplog entry with
        // {"o": {$wouldChangeOwningShard: 1}} written by an external client through the
        // appendOplogNote command.
        operation.setObject2(BSONObj());

        // Required by chunk migration and resharding.
        invariant(wouldChangeOwningShardInfo->getNs());
        invariant(wouldChangeOwningShardInfo->getUuid());
        operation.setNss(*wouldChangeOwningShardInfo->getNs());
        operation.setUuid(*wouldChangeOwningShardInfo->getUuid());
        ShardingWriteRouter shardingWriteRouter(
            opCtx, *wouldChangeOwningShardInfo->getNs(), Grid::get(opCtx)->catalogCache());
        operation.setDestinedRecipient(shardingWriteRouter.getReshardingDestinedRecipient(
            wouldChangeOwningShardInfo->getPreImage()));

        // Required by chunk migration.
        invariant(wouldChangeOwningShardInfo->getNs());
        operation.setNss(*wouldChangeOwningShardInfo->getNs());

        // The operation that triggers WouldChangeOwningShard should always be the first in its
        // transaction.
        operation.setInitializedStatementIds({0});

        addTransactionOperation(opCtx, operation);
    }
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
                                           participant.o().activeTxnNumberAndRetryCounter,
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

    onPrimaryTransactionalWrite.execute([&](const BSONObj& data) {
        const auto closeConnectionElem = data["closeConnection"];
        if (closeConnectionElem.eoo() || closeConnectionElem.Bool()) {
            opCtx->getClient()->session()->end();
        }

        const auto failBeforeCommitExceptionElem = data["failBeforeCommitExceptionCode"];
        if (!failBeforeCommitExceptionElem.eoo()) {
            const auto failureCode = ErrorCodes::Error(int(failBeforeCommitExceptionElem.Number()));
            uasserted(failureCode,
                      str::stream() << "Failing write for " << _sessionId() << ":"
                                    << o().activeTxnNumberAndRetryCounter.getTxnNumber()
                                    << " due to failpoint. The write must not be reflected.");
        }
    });
}

}  // namespace mongo
