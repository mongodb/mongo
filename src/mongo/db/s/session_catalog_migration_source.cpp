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

#include "mongo/db/s/session_catalog_migration_source.h"

#include <memory>

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/session_catalog_migration.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/platform/random.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

struct LastTxnSession {
    LogicalSessionId sessionId;
    TxnNumber txnNumber;
};

boost::optional<repl::OplogEntry> forgeNoopEntryFromImageCollection(
    OperationContext* opCtx, const repl::OplogEntry retryableFindAndModifyOplogEntry) {
    invariant(retryableFindAndModifyOplogEntry.getNeedsRetryImage());

    DBDirectClient client(opCtx);
    BSONObj imageObj =
        client.findOne(NamespaceString::kConfigImagesNamespace,
                       BSON("_id" << retryableFindAndModifyOplogEntry.getSessionId()->toBSON()));
    if (imageObj.isEmpty()) {
        return boost::none;
    }

    auto image = repl::ImageEntry::parse(IDLParserContext("image entry"), imageObj);
    if (image.getTxnNumber() != retryableFindAndModifyOplogEntry.getTxnNumber()) {
        // In our snapshot, fetch the current transaction number for a session. If that transaction
        // number doesn't match what's found on the image lookup, it implies that the image is not
        // the correct version for this oplog entry. We will not forge a noop from it.
        return boost::none;
    }

    repl::MutableOplogEntry forgedNoop;
    forgedNoop.setSessionId(image.get_id());
    forgedNoop.setTxnNumber(image.getTxnNumber());
    forgedNoop.setObject(image.getImage());
    forgedNoop.setOpType(repl::OpTypeEnum::kNoop);
    // The wallclock and namespace are not directly available on the txn document when
    // forging the noop image document.
    forgedNoop.setWallClockTime(Date_t::now());
    forgedNoop.setNss(retryableFindAndModifyOplogEntry.getNss());
    forgedNoop.setUuid(retryableFindAndModifyOplogEntry.getUuid());
    // The OpTime is probably the last write time, but the destination will overwrite this
    // anyways. Just set an OpTime to satisfy the IDL constraints for calling `toBSON`.
    repl::OpTimeBase opTimeBase(Timestamp::min());
    opTimeBase.setTerm(-1);
    forgedNoop.setOpTimeBase(opTimeBase);
    forgedNoop.setStatementIds(retryableFindAndModifyOplogEntry.getStatementIds());
    forgedNoop.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp::min(), -1));
    return repl::OplogEntry::parse(forgedNoop.toBSON()).getValue();
}

boost::optional<repl::OplogEntry> fetchPrePostImageOplog(OperationContext* opCtx,
                                                         repl::OplogEntry* oplog) {
    if (oplog->getNeedsRetryImage()) {
        auto ret = forgeNoopEntryFromImageCollection(opCtx, *oplog);
        if (ret == boost::none) {
            // No pre/post image was found. Defensively strip the `needsRetryImage` value to remove
            // any notion this operation was a retryable findAndModify. If the request is retried on
            // the destination, it will surface an error to the user.
            auto mutableOplog =
                fassert(5676405, repl::MutableOplogEntry::parse(oplog->getEntry().toBSON()));
            mutableOplog.setNeedsRetryImage(boost::none);
            *oplog = repl::OplogEntry(mutableOplog.toBSON());
        }
        return ret;
    }

    auto opTimeToFetch = oplog->getPreImageOpTime();
    if (!opTimeToFetch) {
        opTimeToFetch = oplog->getPostImageOpTime();
    }

    if (!opTimeToFetch) {
        return boost::none;
    }

    auto opTime = opTimeToFetch.value();
    DBDirectClient client(opCtx);
    auto oplogBSON = client.findOne(NamespaceString::kRsOplogNamespace, opTime.asQuery());

    return uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
}

/**
 * Creates an OplogEntry using the given field values
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                const BSONObj& oField,
                                const boost::optional<BSONObj>& o2Field,
                                const OperationSessionInfo& sessionInfo,
                                Date_t wallClockTime,
                                const std::vector<StmtId>& statementIds) {
    return {
        repl::DurableOplogEntry(opTime,                           // optime
                                opType,                           // op type
                                {},                               // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                repl::OplogEntry::kOplogVersion,  // version
                                oField,                           // o
                                o2Field,                          // o2
                                sessionInfo,                      // session info
                                boost::none,                      // upsert
                                wallClockTime,                    // wall clock time
                                statementIds,                     // statement ids
                                boost::none,    // optime of previous write within same transaction
                                boost::none,    // pre-image optime
                                boost::none,    // post-image optime
                                boost::none,    // ShardId of resharding recipient
                                boost::none,    // _id
                                boost::none)};  // needsRetryImage
}

/**
 * Creates a special "write history lost" sentinel oplog entry.
 */
repl::OplogEntry makeSentinelOplogEntry(const LogicalSessionId& lsid,
                                        const TxnNumber& txnNumber,
                                        Date_t wallClockTime) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    return makeOplogEntry({},                                        // optime
                          repl::OpTypeEnum::kNoop,                   // op type
                          {},                                        // o
                          TransactionParticipant::kDeadEndSentinel,  // o2
                          sessionInfo,                               // session info
                          wallClockTime,                             // wall clock time
                          {kIncompleteHistoryStmtId});               // statement id
}

/**
 * If the given oplog entry is an oplog entry for a retryable internal transaction, returns a copy
 * of it but with the session id and transaction number set to the session id and transaction number
 * of the retryable write that it corresponds to. Otherwise, returns the original oplog entry.
 */
repl::OplogEntry downConvertSessionInfoIfNeeded(const repl::OplogEntry& oplogEntry) {
    const auto sessionId = oplogEntry.getSessionId();
    if (isInternalSessionForRetryableWrite(*sessionId)) {
        auto mutableOplogEntry =
            fassert(6349401, repl::MutableOplogEntry::parse(oplogEntry.getEntry().toBSON()));
        mutableOplogEntry.setSessionId(*getParentSessionId(*sessionId));
        mutableOplogEntry.setTxnNumber(*sessionId->getTxnNumber());

        return {mutableOplogEntry.toBSON()};
    }
    return oplogEntry;
}

}  // namespace

SessionCatalogMigrationSource::SessionCatalogMigrationSource(OperationContext* opCtx,
                                                             NamespaceString ns,
                                                             ChunkRange chunk,
                                                             KeyPattern shardKey)
    : _ns(std::move(ns)),
      _rollbackIdAtInit(repl::ReplicationProcess::get(opCtx)->getRollbackID()),
      _chunkRange(std::move(chunk)),
      _keyPattern(shardKey) {

    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    // Skip internal sessions for retryable writes with aborted or in progress transactions since
    // there is no write history to transfer at this point. Skip all internal sessions for
    // non-retryable writes since they only support transactions and those transactions are not
    // retryable so there is no need to transfer their write history to the recipient.
    findRequest.setFilter(BSON(
        "$or" << BSON_ARRAY(
            BSON((SessionTxnRecord::kSessionIdFieldName + "." + LogicalSessionId::kTxnUUIDFieldName)
                 << BSON("$exists" << false))
            << BSON("$and" << BSON_ARRAY(BSON((SessionTxnRecord::kSessionIdFieldName + "." +
                                               LogicalSessionId::kTxnNumberFieldName)
                                              << BSON("$exists" << true)
                                              << SessionTxnRecord::kStateFieldName
                                              << "committed"))))));
    // Sort the records in descending of the session id (_id) field so that the records for internal
    // sessions with highest txnNumber are returned first. This enables us to avoid migrating
    // internal sessions for retryable writes with txnNumber lower than the highest txnNumber.
    findRequest.setSort(BSON(SessionTxnRecord::kSessionIdFieldName << -1));
    auto cursor = client.find(std::move(findRequest));

    boost::optional<LastTxnSession> lastTxnSession;
    while (cursor->more()) {
        const auto txnRecord =
            SessionTxnRecord::parse(IDLParserContext("Session migration cloning"), cursor->next());

        const auto sessionId = txnRecord.getSessionId();
        const auto parentSessionId = castToParentSessionId(sessionId);
        const auto parentTxnNumber =
            sessionId.getTxnNumber() ? *sessionId.getTxnNumber() : txnRecord.getTxnNum();

        if (lastTxnSession && (lastTxnSession->sessionId == parentSessionId) &&
            (lastTxnSession->txnNumber > parentTxnNumber)) {
            // Skip internal sessions for retryable writes with txnNumber lower than the higest
            // txnNumber.
            continue;
        }
        lastTxnSession = LastTxnSession{parentSessionId, parentTxnNumber};

        if (!txnRecord.getLastWriteOpTime().isNull()) {
            _sessionOplogIterators.push_back(
                std::make_unique<SessionOplogIterator>(std::move(txnRecord), _rollbackIdAtInit));
        }
    }

    {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
        writeConflictRetry(
            opCtx,
            "session migration initialization majority commit barrier",
            NamespaceString::kRsOplogNamespace.ns(),
            [&] {
                const auto message = BSON("sessionMigrateCloneStart" << _ns.ns());

                WriteUnitOfWork wuow(opCtx);
                opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
                    opCtx,
                    _ns,
                    {},
                    {},
                    message,
                    boost::none,
                    boost::none,
                    boost::none,
                    boost::none);
                wuow.commit();
            });
    }

    auto opTimeToWait = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult result;
    WriteConcernOptions majority{WriteConcernOptions::kMajority,
                                 WriteConcernOptions::SyncMode::UNSET,
                                 WriteConcernOptions::kNoTimeout};
    uassertStatusOK(waitForWriteConcern(opCtx, opTimeToWait, majority, &result));

    AutoGetCollection collection(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IS);
    // Session docs contain at least LSID, TxnNumber, Timestamp, and some BSON overhead.
    const int64_t defaultSessionDocSize =
        sizeof(LogicalSessionId) + sizeof(TxnNumber) + sizeof(Timestamp) + 16;
    _averageSessionDocSize =
        collection ? collection->averageObjectSize(opCtx) : defaultSessionDocSize;
}

bool SessionCatalogMigrationSource::hasMoreOplog() {
    if (_hasMoreOplogFromSessionCatalog()) {
        return true;
    }

    stdx::lock_guard<Latch> lk(_newOplogMutex);
    return _hasNewWrites(lk);
}

bool SessionCatalogMigrationSource::inCatchupPhase() {
    return !_hasMoreOplogFromSessionCatalog();
}

int64_t SessionCatalogMigrationSource::untransferredCatchUpDataSize() {
    invariant(inCatchupPhase());
    return _newWriteOpTimeList.size() * _averageSessionDocSize;
}

void SessionCatalogMigrationSource::onCommitCloneStarted() {
    stdx::lock_guard<Latch> _lk(_newOplogMutex);

    _state = State::kCommitStarted;
    if (_newOplogNotification) {
        _newOplogNotification->set(true);
        _newOplogNotification.reset();
    }
}

void SessionCatalogMigrationSource::onCloneCleanup() {
    stdx::lock_guard<Latch> _lk(_newOplogMutex);

    _state = State::kCleanup;
    if (_newOplogNotification) {
        _newOplogNotification->set(true);
        _newOplogNotification.reset();
    }
}

SessionCatalogMigrationSource::OplogResult SessionCatalogMigrationSource::getLastFetchedOplog() {
    {
        stdx::lock_guard<Latch> _lk(_sessionCloneMutex);
        if (_lastFetchedOplogImage) {
            return OplogResult(_lastFetchedOplogImage, false);
        } else if (_lastFetchedOplog) {
            return OplogResult(_lastFetchedOplog, false);
        }
    }

    {
        stdx::lock_guard<Latch> _lk(_newOplogMutex);
        if (_lastFetchedNewWriteOplogImage) {
            return OplogResult(_lastFetchedNewWriteOplogImage, false);
        }
        return OplogResult(_lastFetchedNewWriteOplog, true);
    }
}

bool SessionCatalogMigrationSource::fetchNextOplog(OperationContext* opCtx) {
    if (_fetchNextOplogFromSessionCatalog(opCtx)) {
        return true;
    }

    return _fetchNextNewWriteOplog(opCtx);
}

std::shared_ptr<Notification<bool>> SessionCatalogMigrationSource::getNotificationForNewOplog() {
    invariant(!_hasMoreOplogFromSessionCatalog());

    stdx::lock_guard<Latch> lk(_newOplogMutex);

    if (_newOplogNotification) {
        return _newOplogNotification;
    }

    auto notification = std::make_shared<Notification<bool>>();
    if (_state == State::kCleanup) {
        notification->set(true);
    }
    // Even if commit has started, we still need to drain the current buffer.
    else if (_hasNewWrites(lk)) {
        notification->set(false);
    } else if (_state == State::kCommitStarted) {
        notification->set(true);
    } else {
        _newOplogNotification = notification;
    }

    return notification;
}

bool SessionCatalogMigrationSource::shouldSkipOplogEntry(const mongo::repl::OplogEntry& oplogEntry,
                                                         const ShardKeyPattern& shardKeyPattern,
                                                         const ChunkRange& chunkRange) {
    if (oplogEntry.isCrudOpType()) {
        auto shardKey = shardKeyPattern.extractShardKeyFromOplogEntry(oplogEntry);
        return !chunkRange.containsKey(shardKey);
    }

    auto object = oplogEntry.getObject();
    auto object2 = oplogEntry.getObject2();

    // We affirm the no-op oplog entry has an 'o2' field to know it was generated by the system
    // internally and couldn't have come from the appendOplogNote command.
    bool isRewrittenNoopOplog = oplogEntry.getOpType() == repl::OpTypeEnum::kNoop && object2 &&
        !object2->isEmpty() && object.binaryEqual(SessionCatalogMigration::kSessionOplogTag);

    if (isRewrittenNoopOplog) {
        bool isIncompleteHistory = !oplogEntry.getStatementIds().empty() &&
            oplogEntry.getStatementIds().front() == kIncompleteHistoryStmtId;

        if (isIncompleteHistory) {
            // $incompleteOplogHistory no-op oplog entries must always be passed along still to
            // prevent a multi-statement transaction from being retried as a retryable write.
            return false;
        }
        auto shardKey = shardKeyPattern.extractShardKeyFromOplogEntry(object2.value());
        return !chunkRange.containsKey(shardKey);
    }

    return false;
}

void SessionCatalogMigrationSource::_extractOplogEntriesForInternalTransactionForRetryableWrite(
    WithLock,
    const repl::OplogEntry& applyOpsOplogEntry,
    std::vector<repl::OplogEntry>* oplogBuffer) {
    invariant(isInternalSessionForRetryableWrite(*applyOpsOplogEntry.getSessionId()));
    invariant(applyOpsOplogEntry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);

    auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(applyOpsOplogEntry.getObject());
    auto unrolledOp =
        uassertStatusOK(repl::MutableOplogEntry::parse(applyOpsOplogEntry.getEntry().toBSON()));

    for (const auto& innerOp : applyOpsInfo.getOperations()) {
        auto replOp = repl::ReplOperation::parse(
            IDLParserContext{"SessionOplogIterator::_"
                             "extractOplogEntriesForInternalTransactionForRetryableWrite"},
            innerOp);

        if (replOp.getStatementIds().empty()) {
            // Skip this operation since it is not retryable.
            continue;
        }

        if (replOp.getNss() != _ns) {
            // Skip this operation since it does not involve the namespace being migrated.
            continue;
        }

        unrolledOp.setDurableReplOperation(replOp);
        auto unrolledOplogEntry = repl::OplogEntry(unrolledOp.toBSON());

        if (shouldSkipOplogEntry(unrolledOplogEntry, _keyPattern, _chunkRange)) {
            continue;
        }

        oplogBuffer->emplace_back(unrolledOplogEntry);
    }
}

bool SessionCatalogMigrationSource::_handleWriteHistory(WithLock lk, OperationContext* opCtx) {
    while (_currentOplogIterator) {
        if (_unprocessedOplogBuffer.empty()) {
            // The oplog buffer is empty. Fetch the next oplog entry from the current session
            // oplog iterator.
            auto nextOplog = _currentOplogIterator->getNext(opCtx);

            if (!nextOplog) {
                _currentOplogIterator.reset();
                return false;
            }

            // Determine if this oplog entry should be migrated. If so, add the oplog entry or the
            // oplog entries derived from it to the oplog buffer.

            if (isInternalSessionForRetryableWrite(*nextOplog->getSessionId())) {
                invariant(nextOplog->getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
                // Derive retryable write oplog entries from this retryable internal transaction
                // applyOps oplog entry, and add them to the oplog buffer.
                _extractOplogEntriesForInternalTransactionForRetryableWrite(
                    lk, *nextOplog, &_unprocessedOplogBuffer);
                continue;
            }

            // We only expect to see two kinds of oplog entries here:
            // - Dead-end sentinel oplog entries which by design should have stmtId equal to
            //   kIncompleteHistoryStmtId.
            // - CRUD or noop oplog entries for retryable writes which by design should have a
            //   stmtId.
            auto nextStmtIds = nextOplog->getStatementIds();
            invariant(!nextStmtIds.empty());

            // Skip the rest of the chain for this session since the ns is unrelated with the
            // current one being migrated. It is ok to not check the rest of the chain because
            // retryable writes doesn't allow touching different namespaces.
            if (nextStmtIds.front() != kIncompleteHistoryStmtId && nextOplog->getNss() != _ns) {
                _currentOplogIterator.reset();
                return false;
            }

            // Skipping an entry here will also result in the pre/post images to also not be
            // sent in the migration as they're handled by 'fetchPrePostImageOplog' below.
            if (shouldSkipOplogEntry(nextOplog.value(), _keyPattern, _chunkRange)) {
                continue;
            }

            _unprocessedOplogBuffer.emplace_back(*nextOplog);
        }

        // Peek the next oplog entry in the buffer and process it. We cannot pop the oplog
        // entry upfront since it may require fetching/forging a pre or post image and the reads
        // done as part of that can fail with a WriteConflictException error.
        auto nextOplog = _unprocessedOplogBuffer.back();
        auto nextImageOplog = fetchPrePostImageOplog(opCtx, &nextOplog);
        invariant(!_lastFetchedOplogImage);
        invariant(!_lastFetchedOplog);
        if (nextImageOplog) {
            _lastFetchedOplogImage = downConvertSessionInfoIfNeeded(*nextImageOplog);
        }
        _lastFetchedOplog = downConvertSessionInfoIfNeeded(nextOplog);
        _unprocessedOplogBuffer.pop_back();
        return true;
    }

    return false;
}

bool SessionCatalogMigrationSource::_hasMoreOplogFromSessionCatalog() {
    stdx::lock_guard<Latch> _lk(_sessionCloneMutex);
    return _lastFetchedOplog || !_unprocessedOplogBuffer.empty() ||
        !_sessionOplogIterators.empty() || _currentOplogIterator;
}

bool SessionCatalogMigrationSource::_fetchNextOplogFromSessionCatalog(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_sessionCloneMutex);

    if (_lastFetchedOplogImage) {
        // When `_lastFetchedOplogImage` is set, it means we found an oplog entry with a pre/post
        // image. At this step, we've already returned the image oplog entry, but we have yet to
        // return the original oplog entry stored in `_lastFetchedOplog`. We will unset this value
        // and return such that the next call to `getLastFetchedOplog` will return
        // `_lastFetchedOplog`.
        _lastFetchedOplogImage.reset();
        return true;
    }

    _lastFetchedOplog.reset();

    if (_handleWriteHistory(lk, opCtx)) {
        return true;
    }

    while (!_sessionOplogIterators.empty()) {
        _currentOplogIterator = std::move(_sessionOplogIterators.back());
        _sessionOplogIterators.pop_back();

        if (_handleWriteHistory(lk, opCtx)) {
            return true;
        }
    }

    return false;
}

bool SessionCatalogMigrationSource::_hasNewWrites(WithLock) {
    return _lastFetchedNewWriteOplog || !_newWriteOpTimeList.empty() ||
        !_unprocessedNewWriteOplogBuffer.empty();
}

bool SessionCatalogMigrationSource::_fetchNextNewWriteOplog(OperationContext* opCtx) {
    boost::optional<repl::OplogEntry> nextNewWriteOplog;

    {
        stdx::unique_lock<Latch> lk(_newOplogMutex);

        if (_lastFetchedNewWriteOplogImage) {
            // When `_lastFetchedNewWriteOplogImage` is set, it means we found an oplog entry with
            // a pre/post image. At this step, we've already returned the image oplog entry, but we
            // have yet to return the original oplog entry stored in `_lastFetchedNewWriteOplog`. We
            // will unset this value and return such that the next call to `getLastFetchedOplog`
            // will return `_lastFetchedNewWriteOplog`.
            _lastFetchedNewWriteOplogImage.reset();
            return true;
        }

        _lastFetchedNewWriteOplog.reset();

        if (_unprocessedNewWriteOplogBuffer.empty() && _newWriteOpTimeList.empty()) {
            return false;
        }

        if (_unprocessedNewWriteOplogBuffer.empty()) {
            // The oplog buffer is empty. Peek the next opTime and fetch its oplog entry while not
            // holding the mutex. We cannot dequeue the opTime upfront since the the read can fail
            // with a WriteConflictException error.
            repl::OpTime opTimeToFetch;
            EntryAtOpTimeType entryAtOpTimeType;
            std::tie(opTimeToFetch, entryAtOpTimeType) = _newWriteOpTimeList.front();

            lk.unlock();
            DBDirectClient client(opCtx);
            const auto& nextNewWriteOplogDoc =
                client.findOne(NamespaceString::kRsOplogNamespace, opTimeToFetch.asQuery());
            uassert(40620,
                    str::stream() << "Unable to fetch oplog entry with opTime: "
                                  << opTimeToFetch.toBSON(),
                    !nextNewWriteOplogDoc.isEmpty());
            auto nextNewWriteOplog = uassertStatusOK(repl::OplogEntry::parse(nextNewWriteOplogDoc));
            lk.lock();

            // Determine how this oplog entry should be migrated. Either add the oplog entry or the
            // oplog entries derived from it to the oplog buffer. Finally, dequeue the opTime.

            if (entryAtOpTimeType == EntryAtOpTimeType::kRetryableWrite) {
                _unprocessedNewWriteOplogBuffer.emplace_back(nextNewWriteOplog);
                _newWriteOpTimeList.pop_front();
            } else if (entryAtOpTimeType == EntryAtOpTimeType::kTransaction) {
                invariant(nextNewWriteOplog.getCommandType() ==
                          repl::OplogEntry::CommandType::kApplyOps);
                const auto sessionId = *nextNewWriteOplog.getSessionId();

                // The opTimes for transactions inside internal sessions for non-retryable writes
                // should never get added to the opTime queue since those transactions are not
                // retryable so there is no need to transfer their write history to the
                // recipient.
                invariant(!isInternalSessionForNonRetryableWrite(sessionId),
                          "Cannot add op time for a non-retryable internal transaction to the "
                          "session migration op time queue");

                if (isInternalSessionForRetryableWrite(sessionId)) {
                    // Derive retryable write oplog entries from this retryable internal
                    // transaction applyOps oplog entry, and add them to the oplog buffer.
                    _extractOplogEntriesForInternalTransactionForRetryableWrite(
                        lk, nextNewWriteOplog, &_unprocessedNewWriteOplogBuffer);
                    _newWriteOpTimeList.pop_front();

                    if (auto prevOpTime = nextNewWriteOplog.getPrevWriteOpTimeInTransaction();
                        prevOpTime && !prevOpTime->isNull()) {
                        // Add the opTime for the previous applyOps oplog entry in the transaction
                        // to the queue.
                        _notifyNewWriteOpTime(lk, *prevOpTime, EntryAtOpTimeType::kTransaction);
                    }

                    lk.unlock();
                    return _fetchNextNewWriteOplog(opCtx);
                }

                // This applyOps oplog entry corresponds to non-internal transaction prepare/commit,
                // replace it with a dead-end sentinel oplog entry.
                auto sentinelOplogEntry =
                    makeSentinelOplogEntry(sessionId,
                                           *nextNewWriteOplog.getTxnNumber(),
                                           opCtx->getServiceContext()->getFastClockSource()->now());
                _unprocessedNewWriteOplogBuffer.emplace_back(sentinelOplogEntry);
                _newWriteOpTimeList.pop_front();
            } else {
                MONGO_UNREACHABLE;
            }
        }

        // Peek the next oplog entry in the buffer and process it below. We cannot pop the oplog
        // entry upfront since it may require fetching/forging a pre or post image and the reads
        // done as part of that can fail with a WriteConflictException error.
        nextNewWriteOplog = _unprocessedNewWriteOplogBuffer.back();
    }

    auto nextNewWriteImageOplog = fetchPrePostImageOplog(opCtx, &(*nextNewWriteOplog));
    {
        stdx::lock_guard<Latch> lk(_newOplogMutex);
        invariant(!_lastFetchedNewWriteOplogImage);
        invariant(!_lastFetchedNewWriteOplog);
        if (nextNewWriteImageOplog) {
            _lastFetchedNewWriteOplogImage =
                downConvertSessionInfoIfNeeded(*nextNewWriteImageOplog);
        }
        _lastFetchedNewWriteOplog = downConvertSessionInfoIfNeeded(*nextNewWriteOplog);
        _unprocessedNewWriteOplogBuffer.pop_back();
    }

    return true;
}

void SessionCatalogMigrationSource::notifyNewWriteOpTime(repl::OpTime opTime,
                                                         EntryAtOpTimeType entryAtOpTimeType) {
    stdx::lock_guard<Latch> lk(_newOplogMutex);
    _notifyNewWriteOpTime(lk, opTime, entryAtOpTimeType);
}

void SessionCatalogMigrationSource::_notifyNewWriteOpTime(WithLock,
                                                          repl::OpTime opTime,
                                                          EntryAtOpTimeType entryAtOpTimeType) {
    _newWriteOpTimeList.emplace_back(opTime, entryAtOpTimeType);

    if (_newOplogNotification) {
        _newOplogNotification->set(false);
        _newOplogNotification.reset();
    }
}

SessionCatalogMigrationSource::SessionOplogIterator::SessionOplogIterator(
    SessionTxnRecord txnRecord, int expectedRollbackId)
    : _record(std::move(txnRecord)), _initialRollbackId(expectedRollbackId), _entryType([&] {
          if (isInternalSessionForRetryableWrite(_record.getSessionId())) {
              // The SessionCatalogMigrationSource should not try to create a SessionOplogIterator
              // for a retryable internal transaction that has aborted or is still in progress or
              // prepare.
              invariant(_record.getState() == DurableTxnStateEnum::kCommitted);
              return EntryType::kRetryableTransaction;
          }
          // The SessionCatalogMigrationSource should not try to create a SessionOplogIterator for
          // internal sessions for non-retryable writes.
          invariant(isParentSessionId(txnRecord.getSessionId()));
          return _record.getState() ? EntryType::kNonRetryableTransaction
                                    : EntryType::kRetryableWrite;
      }()) {
    _writeHistoryIterator =
        std::make_unique<TransactionHistoryIterator>(_record.getLastWriteOpTime());
}

boost::optional<repl::OplogEntry> SessionCatalogMigrationSource::SessionOplogIterator::getNext(
    OperationContext* opCtx) {
    if (!_writeHistoryIterator || !_writeHistoryIterator->hasNext()) {
        return boost::none;
    }

    try {
        uassert(ErrorCodes::IncompleteTransactionHistory,
                str::stream() << "Cannot migrate multi-statement transaction state",
                _entryType == SessionOplogIterator::EntryType::kRetryableWrite ||
                    _entryType == SessionOplogIterator::EntryType::kRetryableTransaction);

        // Note: during SessionCatalogMigrationSource::init, we inserted a document and wait for it
        // to committed to the majority. In addition, the TransactionHistoryIterator uses OpTime
        // to query for the oplog. This means that if we can successfully fetch the oplog, we are
        // guaranteed that they are majority committed. If we can't fetch the oplog, it can either
        // mean that the oplog has been rolled over or was rolled back.
        auto nextOplog = _writeHistoryIterator->next(opCtx);

        if (_entryType == SessionOplogIterator::EntryType::kRetryableTransaction) {
            if (nextOplog.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction) {
                return getNext(opCtx);
            }

            invariant(nextOplog.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        }
        return nextOplog;
    } catch (const AssertionException& excep) {
        if (excep.code() == ErrorCodes::IncompleteTransactionHistory) {
            // Note: no need to check if in replicaSet mode because having an iterator implies
            // oplog exists.
            auto rollbackId = repl::ReplicationProcess::get(opCtx)->getRollbackID();

            uassert(40656,
                    str::stream() << "rollback detected, rollbackId was " << _initialRollbackId
                                  << " but is now " << rollbackId,
                    rollbackId == _initialRollbackId);

            // If the rollbackId hasn't changed, and this record corresponds to a retryable write,
            // this means that the oplog has been truncated, so we return a sentinel oplog entry
            // indicating that the history for the retryable write has been lost. We also return
            // this sentinel entry for prepared or committed transaction records, since we don't
            // support retrying entire transactions.
            //
            // Otherwise, skip the record by returning boost::none.
            auto result = [&]() -> boost::optional<repl::OplogEntry> {
                if (!_record.getState() ||
                    _record.getState().value() == DurableTxnStateEnum::kCommitted ||
                    _record.getState().value() == DurableTxnStateEnum::kPrepared) {
                    return makeSentinelOplogEntry(
                        _record.getSessionId(),
                        _record.getTxnNum(),
                        opCtx->getServiceContext()->getFastClockSource()->now());
                } else {
                    return boost::none;
                }
            }();

            // Reset the iterator so that subsequent calls to getNext() will return boost::none.
            _writeHistoryIterator.reset();

            return result;
        }
        throw;
    }
}

}  // namespace mongo
