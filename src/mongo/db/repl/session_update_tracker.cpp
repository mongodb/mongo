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

#include "mongo/db/repl/session_update_tracker.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
namespace {

/**
 * Creates an oplog entry to perform an update on the transaction table.
 */
OplogEntry createOplogEntryForTransactionTableUpdate(repl::OpTime opTime,
                                                     const BSONObj& updateBSON,
                                                     const BSONObj& o2Field,
                                                     Date_t wallClockTime) {
    return {repl::DurableOplogEntry(opTime,
                                    boost::none,  // hash
                                    repl::OpTypeEnum::kUpdate,
                                    NamespaceString::kSessionTransactionsTableNamespace,
                                    boost::none,  // uuid
                                    false,        // fromMigrate
                                    repl::OplogEntry::kOplogVersion,
                                    updateBSON,
                                    o2Field,
                                    {},    // sessionInfo
                                    true,  // upsert
                                    wallClockTime,
                                    {},             // statementIds
                                    boost::none,    // prevWriteOpTime
                                    boost::none,    // preImageOpTime
                                    boost::none,    // postImageOpTime
                                    boost::none,    // destinedRecipient
                                    boost::none,    // _id
                                    boost::none)};  // needsRetryImage
}

/**
 * Constructs a new oplog entry if the given entry has transaction state embedded within in. The new
 * oplog entry will contain the operation needed to replicate the transaction table.
 *
 * Returns boost::none if the given oplog doesn't have any transaction state or does not support
 * update to the transaction table.
 */
boost::optional<repl::OplogEntry> createMatchingTransactionTableUpdate(
    const repl::OplogEntry& entry) {
    auto sessionInfo = entry.getOperationSessionInfo();
    if (!sessionInfo.getTxnNumber()) {
        return boost::none;
    }

    invariant(sessionInfo.getSessionId());

    const auto updateBSON = [&] {
        SessionTxnRecord newTxnRecord;
        const auto lsid = *sessionInfo.getSessionId();
        newTxnRecord.setSessionId(lsid);
        if (isInternalSessionForRetryableWrite(lsid)) {
            newTxnRecord.setParentSessionId(*getParentSessionId(lsid));
        }
        newTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
        newTxnRecord.setTxnRetryCounter(sessionInfo.getTxnRetryCounter());
        newTxnRecord.setLastWriteOpTime(entry.getOpTime());
        newTxnRecord.setLastWriteDate(entry.getWallClockTime());

        return newTxnRecord.toBSON();
    }();

    return createOplogEntryForTransactionTableUpdate(
        entry.getOpTime(),
        updateBSON,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON()),
        entry.getWallClockTime());
}

/**
 * A tenant migrations transaction entry will:
 *
 * 1) Have the 'fromTenantMigration' field set
 * 2) Be a no-op entry
 * 3) Have sessionId and txnNumber
 */
bool isTransactionEntryFromTenantMigrations(const OplogEntry& entry) {
    if (!entry.getFromTenantMigration()) {
        return false;
    }

    if (entry.getFromMigrate()) {
        // Retryable writes have fromMigrate set.
        return false;
    }

    if (entry.getOpType() != repl::OpTypeEnum::kNoop) {
        return false;
    }

    if (!entry.getSessionId() || !entry.getTxnNumber()) {
        return false;
    }

    return true;
}

}  // namespace

bool SessionUpdateTracker::isTransactionEntry(const OplogEntry& entry) {
    if (isTransactionEntryFromTenantMigrations(entry)) {
        return true;
    }

    auto sessionInfo = entry.getOperationSessionInfo();
    if (!sessionInfo.getTxnNumber()) {
        return false;
    }

    return entry.isPartialTransaction() ||
        entry.getCommandType() == repl::OplogEntry::CommandType::kAbortTransaction ||
        entry.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction ||
        entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps;
}

boost::optional<std::vector<OplogEntry>> SessionUpdateTracker::_updateOrFlush(
    const OplogEntry& entry) {
    const auto& ns = entry.getNss();

    if (ns == NamespaceString::kSessionTransactionsTableNamespace ||
        (ns.isConfigDB() && ns.isCommand())) {
        return _flush(entry);
    }

    return _updateSessionInfo(entry);
}

boost::optional<std::vector<OplogEntry>> SessionUpdateTracker::updateSession(
    const OplogEntry& entry) {
    if (!isTransactionEntry(entry)) {
        return _updateOrFlush(entry);
    }

    // If we generate an update from a multi-statement transaction operation, we must clear (then
    // replace) a possibly queued transaction table update for a retryable write on this session.
    // It is okay to clear the transaction table update because retryable writes only care about
    // the final state of the transaction table entry for a given session, not the full history
    // of updates for the session. By contrast, we care about each transaction table update for
    // multi-statement transactions -- we must maintain the timestamps and transaction states for
    // each entry originating from a multi-statement transaction. For this reason, we cannot defer
    // entries originating from multi-statement transactions.
    if (auto txnTableUpdate = _createTransactionTableUpdateFromTransactionOp(entry)) {
        _sessionsToUpdate.erase(*entry.getOperationSessionInfo().getSessionId());
        return boost::optional<std::vector<OplogEntry>>({*txnTableUpdate});
    }

    return boost::none;
}

boost::optional<std::vector<OplogEntry>> SessionUpdateTracker::_updateSessionInfo(
    const OplogEntry& entry) {
    const auto& sessionInfo = entry.getOperationSessionInfo();

    if (!sessionInfo.getTxnNumber()) {
        return {};
    }

    const auto& lsid = sessionInfo.getSessionId();
    invariant(lsid);

    // Ignore pre/post image no-op oplog entries. These entries will not have an o2 field.
    if (entry.getOpType() == OpTypeEnum::kNoop) {
        if (!entry.getFromMigrate() || !*entry.getFromMigrate()) {
            return {};
        }

        if (!entry.getObject2()) {
            return {};
        }
    }

    auto iter = _sessionsToUpdate.find(*lsid);
    if (iter == _sessionsToUpdate.end()) {
        _sessionsToUpdate.emplace(*lsid, entry);
        return {};
    }

    const auto& existingSessionInfo = iter->second.getOperationSessionInfo();
    const auto existingTxnNumber = *existingSessionInfo.getTxnNumber();
    if (*sessionInfo.getTxnNumber() == existingTxnNumber) {
        iter->second = entry;
        return {};
    }

    if (*sessionInfo.getTxnNumber() > existingTxnNumber) {
        // Do not coalesce updates across txn numbers. For more details, see SERVER-55305.
        auto updateOplog = createMatchingTransactionTableUpdate(iter->second);
        invariant(updateOplog);
        iter->second = entry;
        return std::vector<OplogEntry>{std::move(*updateOplog)};
    }

    LOGV2_FATAL_NOTRACE(50843,
                        "Entry for session {lsid} has txnNumber {sessionInfo_getTxnNumber} < "
                        "{existingSessionInfo_getTxnNumber}. New oplog entry: {newEntry}, Existing "
                        "oplog entry: {existingEntry}",
                        "lsid"_attr = lsid->toBSON(),
                        "sessionInfo_getTxnNumber"_attr = *sessionInfo.getTxnNumber(),
                        "existingSessionInfo_getTxnNumber"_attr = existingTxnNumber,
                        "newEntry"_attr = redact(entry.toBSONForLogging()),
                        "existingEntry"_attr = redact(iter->second.toBSONForLogging()));
}

std::vector<OplogEntry> SessionUpdateTracker::_flush(const OplogEntry& entry) {
    switch (entry.getOpType()) {
        case OpTypeEnum::kInsert:
        case OpTypeEnum::kNoop:
            // Session table is keyed by session id, so nothing to do here because
            // it would have triggered a unique index violation in the primary if
            // it was trying to insert with the same session id with existing ones.
            return {};

        case OpTypeEnum::kUpdate:
            return _flushForQueryPredicate(*entry.getObject2());

        case OpTypeEnum::kDelete:
            return _flushForQueryPredicate(entry.getObject());

        case OpTypeEnum::kCommand:
            return flushAll();
    }

    MONGO_UNREACHABLE;
}

std::vector<OplogEntry> SessionUpdateTracker::flushAll() {
    std::vector<OplogEntry> opList;

    for (auto&& entry : _sessionsToUpdate) {
        auto newUpdate = createMatchingTransactionTableUpdate(entry.second);
        invariant(newUpdate);
        opList.push_back(std::move(*newUpdate));
    }
    _sessionsToUpdate.clear();

    return opList;
}

std::vector<OplogEntry> SessionUpdateTracker::_flushForQueryPredicate(
    const BSONObj& queryPredicate) {
    auto idField = queryPredicate["_id"].Obj();
    auto lsid = LogicalSessionId::parse(IDLParserContext("lsidInOplogQuery"), idField);
    auto iter = _sessionsToUpdate.find(lsid);

    if (iter == _sessionsToUpdate.end()) {
        return {};
    }

    std::vector<OplogEntry> opList;
    auto updateOplog = createMatchingTransactionTableUpdate(iter->second);
    invariant(updateOplog);
    opList.push_back(std::move(*updateOplog));
    _sessionsToUpdate.erase(iter);

    return opList;
}

boost::optional<OplogEntry> SessionUpdateTracker::_createTransactionTableUpdateFromTransactionOp(
    const repl::OplogEntry& entry) {
    auto sessionInfo = entry.getOperationSessionInfo();

    // We only update the transaction table on the first partialTxn operation.
    if (entry.isPartialTransaction() && !entry.getPrevWriteOpTimeInTransaction()->isNull()) {
        return boost::none;
    }
    invariant(sessionInfo.getSessionId());

    const auto updateBSON = [&] {
        SessionTxnRecord newTxnRecord;
        const auto lsid = *sessionInfo.getSessionId();
        newTxnRecord.setSessionId(lsid);
        if (isInternalSessionForRetryableWrite(lsid)) {
            newTxnRecord.setParentSessionId(*getParentSessionId(lsid));
        }
        newTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
        newTxnRecord.setTxnRetryCounter(sessionInfo.getTxnRetryCounter());
        newTxnRecord.setLastWriteOpTime(entry.getOpTime());
        newTxnRecord.setLastWriteDate(entry.getWallClockTime());

        if (entry.getFromTenantMigration() && entry.getOpType() == OpTypeEnum::kNoop) {
            newTxnRecord.setState(DurableTxnStateEnum::kCommitted);
            return newTxnRecord.toBSON();
        }

        if (entry.isPartialTransaction()) {
            invariant(entry.getPrevWriteOpTimeInTransaction()->isNull());
            newTxnRecord.setState(DurableTxnStateEnum::kInProgress);
            newTxnRecord.setStartOpTime(entry.getOpTime());
            return newTxnRecord.toBSON();
        }
        switch (entry.getCommandType()) {
            case repl::OplogEntry::CommandType::kApplyOps:
                if (entry.shouldPrepare()) {
                    newTxnRecord.setState(DurableTxnStateEnum::kPrepared);
                    if (entry.getPrevWriteOpTimeInTransaction()->isNull()) {
                        // The prepare oplog entry is the first operation of the transaction.
                        newTxnRecord.setStartOpTime(entry.getOpTime());
                    } else {
                        // Update the transaction record using a delta oplog entry to avoid
                        // overwriting the startOpTime.
                        return update_oplog_entry::makeDeltaOplogEntry(
                            BSON(doc_diff::kUpdateSectionFieldName << newTxnRecord.toBSON()));
                    }
                } else {
                    newTxnRecord.setState(DurableTxnStateEnum::kCommitted);
                }
                break;
            case repl::OplogEntry::CommandType::kCommitTransaction:
                newTxnRecord.setState(DurableTxnStateEnum::kCommitted);
                break;
            case repl::OplogEntry::CommandType::kAbortTransaction:
                newTxnRecord.setState(DurableTxnStateEnum::kAborted);
                break;
            default:
                break;
        }
        return newTxnRecord.toBSON();
    }();

    return createOplogEntryForTransactionTableUpdate(
        entry.getOpTime(),
        updateBSON,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON()),
        entry.getWallClockTime());
}

}  // namespace repl
}  // namespace mongo
