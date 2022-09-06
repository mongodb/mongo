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

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/testing_proctor.h"

#include <fmt/compile.h>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {

// Always notifies prepare conflict waiters when a transaction commits or aborts, even when the
// transaction is not prepared. This should always be enabled if WTPrepareConflictForReads is
// used, which fails randomly. If this is not enabled, no prepare conflicts will be resolved,
// because the recovery unit may not ever actually be in a prepared state.
MONGO_FAIL_POINT_DEFINE(WTAlwaysNotifyPrepareConflictWaiters);

logv2::LogSeverity kSlowTransactionSeverity = logv2::LogSeverity::Debug(1);

MONGO_FAIL_POINT_DEFINE(doUntimestampedWritesForIdempotencyTests);

void handleWriteContextForDebugging(WiredTigerRecoveryUnit& ru, Timestamp& ts) {
    if (ru.gatherWriteContextForDebugging()) {
        BSONObjBuilder builder;

        std::string s;
        StringStackTraceSink sink{s};
        printStackTrace(sink);
        builder.append("stacktrace", s);

        builder.append("timestamp", ts);

        ru.storeWriteContextForDebugging(builder.obj());
    }
}
}  // namespace

AtomicWord<std::int64_t> snapshotTooOldErrorCount{0};

using Section = WiredTigerOperationStats::Section;

std::map<int, std::pair<StringData, Section>> WiredTigerOperationStats::_statNameMap = {
    {WT_STAT_SESSION_BYTES_READ, std::make_pair("bytesRead"_sd, Section::DATA)},
    {WT_STAT_SESSION_BYTES_WRITE, std::make_pair("bytesWritten"_sd, Section::DATA)},
    {WT_STAT_SESSION_LOCK_DHANDLE_WAIT, std::make_pair("handleLock"_sd, Section::WAIT)},
    {WT_STAT_SESSION_READ_TIME, std::make_pair("timeReadingMicros"_sd, Section::DATA)},
    {WT_STAT_SESSION_WRITE_TIME, std::make_pair("timeWritingMicros"_sd, Section::DATA)},
    {WT_STAT_SESSION_LOCK_SCHEMA_WAIT, std::make_pair("schemaLock"_sd, Section::WAIT)},
    {WT_STAT_SESSION_CACHE_TIME, std::make_pair("cache"_sd, Section::WAIT)}};

std::shared_ptr<StorageStats> WiredTigerOperationStats::getCopy() {
    std::shared_ptr<WiredTigerOperationStats> copy = std::make_shared<WiredTigerOperationStats>();
    *copy += *this;
    return copy;
}

void WiredTigerOperationStats::fetchStats(WT_SESSION* session,
                                          const std::string& uri,
                                          const std::string& config) {
    invariant(session);

    WT_CURSOR* c = nullptr;
    const char* cursorConfig = config.empty() ? nullptr : config.c_str();
    int ret = session->open_cursor(session, uri.c_str(), nullptr, cursorConfig, &c);
    uassert(ErrorCodes::CursorNotFound, "Unable to open statistics cursor", ret == 0);

    invariant(c);
    ON_BLOCK_EXIT([&] { c->close(c); });

    const char* desc;
    uint64_t value;
    int32_t key;
    while (c->next(c) == 0 && c->get_key(c, &key) == 0) {
        fassert(51035, c->get_value(c, &desc, nullptr, &value) == 0);
        _stats[key] = WiredTigerUtil::castStatisticsValue<long long>(value);
    }

    // Reset the statistics so that the next fetch gives the recent values.
    invariantWTOK(c->reset(c), c->session);
}

BSONObj WiredTigerOperationStats::toBSON() {
    BSONObjBuilder bob;
    std::unique_ptr<BSONObjBuilder> dataSection;
    std::unique_ptr<BSONObjBuilder> waitSection;

    for (auto const& stat : _stats) {
        // Find the user consumable name for this statistic.
        auto statIt = _statNameMap.find(stat.first);

        // Ignore the session statistic that is not reported for the slow operations.
        if (statIt == _statNameMap.end())
            continue;

        auto statName = statIt->second.first;
        Section subs = statIt->second.second;
        long long val = stat.second;
        // Add this statistic only if higher than zero.
        if (val > 0) {
            // Gather the statistic into its own subsection in the BSONObj.
            switch (subs) {
                case Section::DATA:
                    if (!dataSection)
                        dataSection = std::make_unique<BSONObjBuilder>();

                    dataSection->append(statName, val);
                    break;
                case Section::WAIT:
                    if (!waitSection)
                        waitSection = std::make_unique<BSONObjBuilder>();

                    waitSection->append(statName, val);
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }

    if (dataSection)
        bob.append("data", dataSection->obj());
    if (waitSection)
        bob.append("timeWaitingMicros", waitSection->obj());

    return bob.obj();
}

WiredTigerOperationStats& WiredTigerOperationStats::operator+=(
    const WiredTigerOperationStats& other) {
    for (auto const& otherStat : other._stats) {
        _stats[otherStat.first] += otherStat.second;
    }
    return (*this);
}

StorageStats& WiredTigerOperationStats::operator+=(const StorageStats& other) {
    *this += checked_cast<const WiredTigerOperationStats&>(other);
    return (*this);
}

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc)
    : WiredTigerRecoveryUnit(sc, sc->getKVEngine()->getOplogManager()) {}

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc,
                                               WiredTigerOplogManager* oplogManager)
    : _sessionCache(sc), _oplogManager(oplogManager) {}

WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _abort();
}

void WiredTigerRecoveryUnit::_commit() {
    // Since we cannot have both a _lastTimestampSet and a _commitTimestamp, we set the
    // commit time as whichever is non-empty. If both are empty, then _lastTimestampSet will
    // be boost::none and we'll set the commit time to that.
    auto commitTime = _commitTimestamp.isNull() ? _lastTimestampSet : _commitTimestamp;

    bool notifyDone = !_prepareTimestamp.isNull();
    if (_session && _isActive()) {
        _txnClose(true);
    }
    _setState(State::kCommitting);

    if (MONGO_unlikely(WTAlwaysNotifyPrepareConflictWaiters.shouldFail())) {
        notifyDone = true;
    }

    if (notifyDone) {
        _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
    }

    commitRegisteredChanges(commitTime);
    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::_abort() {
    bool notifyDone = !_prepareTimestamp.isNull();
    if (_session && _isActive()) {
        _txnClose(false);
    }
    _setState(State::kAborting);

    if (notifyDone || MONGO_unlikely(WTAlwaysNotifyPrepareConflictWaiters.shouldFail())) {
        _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
    }

    abortRegisteredChanges();
    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::doBeginUnitOfWork() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!_isCommittingOrAborting(),
              str::stream() << "cannot begin unit of work while commit or rollback handlers are "
                               "running: "
                            << toString(_getState()));
    _setState(_isActive() ? State::kActive : State::kInactiveInUnitOfWork);
}

void WiredTigerRecoveryUnit::prepareUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(!_prepareTimestamp.isNull());

    auto session = getSession();
    WT_SESSION* s = session->getSession();

    LOGV2_DEBUG(22410,
                1,
                "preparing transaction at time: {prepareTimestamp}",
                "prepareTimestamp"_attr = _prepareTimestamp);

    const std::string conf = "prepare_timestamp=" + unsignedHex(_prepareTimestamp.asULL());
    // Prepare the transaction.
    invariantWTOK(s->prepare_transaction(s, conf.c_str()), s);
}

void WiredTigerRecoveryUnit::doCommitUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    _commit();
}

void WiredTigerRecoveryUnit::doAbortUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    _abort();
}

void WiredTigerRecoveryUnit::_ensureSession() {
    if (!_session) {
        _session = _sessionCache->getSession();
    }
}

bool WiredTigerRecoveryUnit::waitUntilDurable(OperationContext* opCtx) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!opCtx->lockState()->isLocked() || storageGlobalParams.repair);

    // Flushes the journal log to disk. Checkpoints all data if journaling is disabled.
    _sessionCache->waitUntilDurable(opCtx,
                                    WiredTigerSessionCache::Fsync::kJournal,
                                    WiredTigerSessionCache::UseJournalListener::kUpdate);

    return true;
}

bool WiredTigerRecoveryUnit::waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                                               bool stableCheckpoint) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!opCtx->lockState()->isLocked() || storageGlobalParams.repair);

    // Take a checkpoint, rather than only flush the (oplog) journal, in order to lock in stable
    // writes to unjournaled tables.
    //
    // If 'stableCheckpoint' is set, then we will only checkpoint data up to and including the
    // stable_timestamp set on WT at the time of the checkpoint. Otherwise, we will checkpoint all
    // of the data.
    WiredTigerSessionCache::Fsync fsyncType = stableCheckpoint
        ? WiredTigerSessionCache::Fsync::kCheckpointStableTimestamp
        : WiredTigerSessionCache::Fsync::kCheckpointAll;
    _sessionCache->waitUntilDurable(
        opCtx, fsyncType, WiredTigerSessionCache::UseJournalListener::kUpdate);

    return true;
}

void WiredTigerRecoveryUnit::assertInActiveTxn() const {
    if (_isActive()) {
        return;
    }
    LOGV2_FATAL(28575,
                "Recovery unit is not active. Current state: {currentState}",
                "Recovery unit is not active.",
                "currentState"_attr = _getState());
}

void WiredTigerRecoveryUnit::setTxnModified() {
    if (_multiTimestampConstraintTracker.isTxnModified) {
        return;
    }

    _multiTimestampConstraintTracker.isTxnModified = true;
    if (!_lastTimestampSet) {
        _multiTimestampConstraintTracker.txnHasNonTimestampedWrite = true;
    }
}

boost::optional<int64_t> WiredTigerRecoveryUnit::getOplogVisibilityTs() {
    if (!_isOplogReader) {
        return boost::none;
    }

    getSession();
    return _oplogVisibleTs;
}

WiredTigerSession* WiredTigerRecoveryUnit::getSession() {
    if (!_isActive()) {
        _txnOpen();
        _setState(_inUnitOfWork() ? State::kActive : State::kActiveNotInUnitOfWork);
    }
    return _session.get();
}

WiredTigerSession* WiredTigerRecoveryUnit::getSessionNoTxn() {
    _ensureSession();
    WiredTigerSession* session = _session.get();

    // Handling queued drops can be slow, which is not desired for internal operations like FTDC
    // sampling. Disable handling of queued drops for such sessions.
    session->dropQueuedIdentsAtSessionEndAllowed(false);
    return session;
}

void WiredTigerRecoveryUnit::doAbandonSnapshot() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    if (_isActive()) {
        // Can't be in a WriteUnitOfWork, so safe to rollback if the AbandonSnapshotMode is
        // kAbort. If kCommit, however, then any active cursors will remain positioned and valid.
        _txnClose(_abandonSnapshotMode == AbandonSnapshotMode::kCommit /* commit */);
    }
    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::preallocateSnapshot() {
    // Begin a new transaction, if one is not already started.
    getSession();
}

void WiredTigerRecoveryUnit::preallocateSnapshotForOplogRead() {
    // Indicate that we are an oplog reader before opening the snapshot
    setIsOplogReader();
    preallocateSnapshot();
}

void WiredTigerRecoveryUnit::_txnClose(bool commit) {
    invariant(_isActive(), toString(_getState()));

    if (TestingProctor::instance().isEnabled() && _gatherWriteContextForDebugging && commit) {
        LOGV2(5703402,
              "Closing transaction with write context for debugging",
              "count"_attr = _writeContextForDebugging.size());
        for (auto const& ctx : _writeContextForDebugging) {
            LOGV2_OPTIONS(5703403,
                          {logv2::LogTruncation::Disabled},
                          "Write context for debugging",
                          "context"_attr = ctx);
        }

        _writeContextForDebugging.clear();
        // We clear the context here, but we don't unset the flag. We need it still set to prevent a
        // WCE loop in the multi-timestamp constraint code below. We are also expecting to hit the
        // LOGV2_FATAL below, and don't really need to worry about re-using this recovery unit. If
        // this changes in the future, we might need to unset _gatherWriteContextForDebugging under
        // some conditions.
    }

    if (!_multiTimestampConstraintTracker.ignoreAllMultiTimestampConstraints &&
        _multiTimestampConstraintTracker.txnHasNonTimestampedWrite &&
        _multiTimestampConstraintTracker.timestampOrder.size() >= 2) {
        // The first write in this transaction was not timestamped. Other writes have used at least
        // two different timestamps. This violates the multi timestamp constraint where if a
        // transaction sets multiple timestamps, the first timestamp must be set prior to any
        // writes. Vice-versa, if a transaction writes a document before setting a timestamp, it
        // must not set multiple timestamps.
        if (TestingProctor::instance().isEnabled() && !_gatherWriteContextForDebugging) {
            _gatherWriteContextForDebugging = true;
            LOGV2_ERROR(5703401,
                        "Found a violation of multi-timestamp constraint. Retrying operation to "
                        "collect extra debugging context for the involved writes.");
            throwWriteConflictException("Violation of multi-timestamp constraint.");
        }
        if (commit) {
            LOGV2_FATAL(
                4877100,
                "Multi timestamp constraint violated. Transactions setting multiple timestamps "
                "must set the first timestamp prior to any writes.",
                "numTimestampsUsed"_attr = _multiTimestampConstraintTracker.timestampOrder.size(),
                "lastSetTimestamp"_attr = _multiTimestampConstraintTracker.timestampOrder.top());
        }
    }

    WT_SESSION* s = _session->getSession();
    if (_timer) {
        const int transactionTime = _timer->millis();
        // `serverGlobalParams.slowMs` can be set to values <= 0. In those cases, give logging a
        // break.
        if (transactionTime >= std::max(1, serverGlobalParams.slowMS)) {
            LOGV2_DEBUG(22411,
                        kSlowTransactionSeverity.toInt(),
                        "Slow WT transaction. Lifetime of SnapshotId {snapshotId} was "
                        "{transactionTime}ms",
                        "snapshotId"_attr = getSnapshotId().toNumber(),
                        "transactionTime"_attr = transactionTime);
        }
    }

    int wtRet;
    if (commit) {
        if (!_commitTimestamp.isNull()) {
            // There is currently no scenario where it is intentional to commit before the current
            // read timestamp.
            invariant(_readAtTimestamp.isNull() || _commitTimestamp >= _readAtTimestamp);

            if (MONGO_likely(!doUntimestampedWritesForIdempotencyTests.shouldFail())) {
                s->timestamp_transaction_uint(s, WT_TS_TXN_TYPE_COMMIT, _commitTimestamp.asULL());
            }
            _isTimestamped = true;
        }

        if (!_durableTimestamp.isNull()) {
            s->timestamp_transaction_uint(s, WT_TS_TXN_TYPE_DURABLE, _durableTimestamp.asULL());
        }

        wtRet = s->commit_transaction(s, nullptr);

        LOGV2_DEBUG(
            22412, 3, "WT commit_transaction", "snapshotId"_attr = getSnapshotId().toNumber());
    } else {
        invariant(_abandonSnapshotMode == AbandonSnapshotMode::kAbort);
        StringBuilder config;
        if (_noEvictionAfterRollback) {
            // The only point at which rollback_transaction() can time out is in the bonus-eviction
            // phase. If the timeout expires here, the function will stop the eviction and return
            // success. It cannot return an error due to timeout.
            config << "operation_timeout_ms=1,";
        }

        wtRet = s->rollback_transaction(s, config.str().c_str());

        LOGV2_DEBUG(
            22413, 3, "WT rollback_transaction", "snapshotId"_attr = getSnapshotId().toNumber());
    }

    if (_isTimestamped) {
        if (!_orderedCommit) {
            // We only need to update oplog visibility where commits can be out-of-order with
            // respect to their assigned optime. This will ensure the oplog read timestamp gets
            // updated when oplog 'holes' are filled: the last commit filling the last hole will
            // prompt the oplog read timestamp to be forwarded.
            //
            // This should happen only on primary nodes.
            _oplogManager->triggerOplogVisibilityUpdate();
        }
        _isTimestamped = false;
    }
    invariantWTOK(wtRet, s);

    invariant(!_lastTimestampSet || _commitTimestamp.isNull(),
              str::stream() << "Cannot have both a _lastTimestampSet and a "
                               "_commitTimestamp. _lastTimestampSet: "
                            << _lastTimestampSet->toString()
                            << ". _commitTimestamp: " << _commitTimestamp.toString());

    // We reset the _lastTimestampSet between transactions. Since it is legal for one
    // transaction on a RecoveryUnit to call setTimestamp() and another to call
    // setCommitTimestamp().
    _lastTimestampSet = boost::none;
    _multiTimestampConstraintTracker = MultiTimestampConstraintTracker();
    _prepareTimestamp = Timestamp();
    _durableTimestamp = Timestamp();
    _catalogConflictTimestamp = Timestamp();
    _roundUpPreparedTimestamps = RoundUpPreparedTimestamps::kNoRound;
    _isOplogReader = false;
    _oplogVisibleTs = boost::none;
    _orderedCommit = true;  // Default value is true; we assume all writes are ordered.
    _untimestampedWriteAssertion = WiredTigerBeginTxnBlock::UntimestampedWriteAssertion::kEnforce;
}

Status WiredTigerRecoveryUnit::majorityCommittedSnapshotAvailable() const {
    invariant(_timestampReadSource == ReadSource::kMajorityCommitted);
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }
    return Status::OK();
}

boost::optional<Timestamp> WiredTigerRecoveryUnit::getPointInTimeReadTimestamp(
    OperationContext* opCtx) {
    // After a ReadSource has been set on this RecoveryUnit, callers expect that this method returns
    // the read timestamp that will be used for current or future transactions. Because callers use
    // this timestamp to inform visibility of operations, it is therefore necessary to open a
    // transaction to establish a read timestamp, but only for ReadSources that are expected to have
    // read timestamps.
    switch (_timestampReadSource) {
        case ReadSource::kNoTimestamp:
        case ReadSource::kCheckpoint:
            return boost::none;
        case ReadSource::kProvided:
            // The read timestamp is set by the user and does not require a transaction to be open.
            invariant(!_readAtTimestamp.isNull());
            return _readAtTimestamp;
        case ReadSource::kLastApplied:
            // The lastApplied timestamp is not always available if the system has not accepted
            // writes, so it is not possible to invariant that it exists.
            if (_readAtTimestamp.isNull()) {
                return boost::none;
            }
            return _readAtTimestamp;
        // The following ReadSources can only establish a read timestamp when a transaction is
        // opened.
        case ReadSource::kNoOverlap:
        case ReadSource::kAllDurableSnapshot:
        case ReadSource::kMajorityCommitted:
            break;
    }

    // Ensure a transaction is opened. Storage engine operations require the global lock.
    invariant(opCtx->lockState()->isNoop() || opCtx->lockState()->isLocked());
    getSession();

    switch (_timestampReadSource) {
        case ReadSource::kLastApplied:
        case ReadSource::kNoOverlap:
            // The lastApplied and allDurable timestamps are not always available if the system has
            // not accepted writes, so it is not possible to invariant that it exists as other
            // ReadSources do.
            if (!_readAtTimestamp.isNull()) {
                return _readAtTimestamp;
            }
            return boost::none;
        case ReadSource::kAllDurableSnapshot:
        case ReadSource::kMajorityCommitted:
            invariant(!_readAtTimestamp.isNull());
            return _readAtTimestamp;

        // The follow ReadSources returned values in the first switch block.
        case ReadSource::kNoTimestamp:
        case ReadSource::kProvided:
        case ReadSource::kCheckpoint:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

void WiredTigerRecoveryUnit::_txnOpen() {
    invariant(!_isActive(), toString(_getState()));
    invariant(!_isCommittingOrAborting(),
              str::stream() << "commit or rollback handler reopened transaction: "
                            << toString(_getState()));
    _ensureSession();

    // Only start a timer for transaction's lifetime if we're going to log it.
    if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, kSlowTransactionSeverity)) {
        _timer.reset(new Timer());
    }
    WT_SESSION* session = _session->getSession();

    switch (_timestampReadSource) {
        case ReadSource::kNoTimestamp: {
            if (_isOplogReader) {
                _oplogVisibleTs = static_cast<std::int64_t>(_oplogManager->getOplogReadTimestamp());
            }
            WiredTigerBeginTxnBlock(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kNoRoundError,
                                    _untimestampedWriteAssertion)
                .done();
            break;
        }
        case ReadSource::kCheckpoint: {
            WiredTigerBeginTxnBlock(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kNoRoundError,
                                    _untimestampedWriteAssertion)
                .done();
            break;
        }
        case ReadSource::kMajorityCommitted: {
            _readAtTimestamp = _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(
                session,
                _prepareConflictBehavior,
                _roundUpPreparedTimestamps,
                _untimestampedWriteAssertion);
            break;
        }
        case ReadSource::kLastApplied: {
            _beginTransactionAtLastAppliedTimestamp(session);
            break;
        }
        case ReadSource::kNoOverlap: {
            _readAtTimestamp = _beginTransactionAtNoOverlapTimestamp(session);
            break;
        }
        case ReadSource::kAllDurableSnapshot: {
            if (_readAtTimestamp.isNull()) {
                _readAtTimestamp = _beginTransactionAtAllDurableTimestamp(session);
                break;
            }
            [[fallthrough]];  // Continue to the next case to read at the _readAtTimestamp.
        }
        case ReadSource::kProvided: {
            WiredTigerBeginTxnBlock txnOpen(session,
                                            _prepareConflictBehavior,
                                            _roundUpPreparedTimestamps,
                                            RoundUpReadTimestamp::kNoRoundError,
                                            _untimestampedWriteAssertion);
            auto status = txnOpen.setReadSnapshot(_readAtTimestamp);

            if (!status.isOK() && status.code() == ErrorCodes::BadValue) {
                // SnapshotTooOld errors indicate that PIT ops are failing to find an available
                // snapshot at their specified atClusterTime.
                snapshotTooOldErrorCount.addAndFetch(1);
                uasserted(ErrorCodes::SnapshotTooOld,
                          str::stream() << "Read timestamp " << _readAtTimestamp.toString()
                                        << " is older than the oldest available timestamp.");
            }
            uassertStatusOK(status);
            txnOpen.done();
            break;
        }
    }

    LOGV2_DEBUG(22414,
                3,
                "WT begin_transaction",
                "snapshotId"_attr = getSnapshotId().toNumber(),
                "readSource"_attr = toString(_timestampReadSource));
}

Timestamp WiredTigerRecoveryUnit::_beginTransactionAtAllDurableTimestamp(WT_SESSION* session) {
    WiredTigerBeginTxnBlock txnOpen(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound,
                                    _untimestampedWriteAssertion);
    Timestamp txnTimestamp = _sessionCache->getKVEngine()->getAllDurableTimestamp();
    auto status = txnOpen.setReadSnapshot(txnTimestamp);
    fassert(50948, status);

    // Since this is not in a critical section, we might have rounded to oldest between
    // calling getAllDurable and setReadSnapshot.  We need to get the actual read timestamp we
    // used.
    auto readTimestamp = _getTransactionReadTimestamp(session);
    txnOpen.done();
    return readTimestamp;
}

void WiredTigerRecoveryUnit::_beginTransactionAtLastAppliedTimestamp(WT_SESSION* session) {
    if (_readAtTimestamp.isNull()) {
        // When there is not a lastApplied timestamp available, read without a timestamp. Do not
        // round up the read timestamp to the oldest timestamp.

        // There is a race that allows new transactions to start between the time we check for a
        // read timestamp and start our transaction, which can temporarily violate the contract of
        // kLastApplied. That is, writes will be visible that occur after the lastApplied time. This
        // is only possible for readers that start immediately after an initial sync that did not
        // replicate any oplog entries. Future transactions will start reading at a timestamp once
        // timestamped writes have been made.
        WiredTigerBeginTxnBlock txnOpen(session,
                                        _prepareConflictBehavior,
                                        _roundUpPreparedTimestamps,
                                        RoundUpReadTimestamp::kNoRoundError,
                                        _untimestampedWriteAssertion);
        LOGV2_DEBUG(4847500, 2, "no read timestamp available for kLastApplied");
        txnOpen.done();
        return;
    }

    WiredTigerBeginTxnBlock txnOpen(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound,
                                    _untimestampedWriteAssertion);
    auto status = txnOpen.setReadSnapshot(_readAtTimestamp);
    fassert(4847501, status);

    // We might have rounded to oldest between calling setTimestampReadSource and setReadSnapshot.
    // We need to get the actual read timestamp we used.
    auto actualTimestamp = _getTransactionReadTimestamp(session);
    txnOpen.done();
    _readAtTimestamp = actualTimestamp;
}

Timestamp WiredTigerRecoveryUnit::_beginTransactionAtNoOverlapTimestamp(WT_SESSION* session) {

    auto lastApplied = _sessionCache->snapshotManager().getLastApplied();
    Timestamp allDurable = Timestamp(_sessionCache->getKVEngine()->getAllDurableTimestamp());

    // When using timestamps for reads and writes, it's important that readers and writers don't
    // overlap with the timestamps they use. In other words, at any point in the system there should
    // be a timestamp T such that writers only commit at times greater than T and readers only read
    // at, or earlier than T. This time T is called the no-overlap point. Using the `kNoOverlap`
    // ReadSource will compute the most recent known time that is safe to read at.

    // The no-overlap point is computed as the minimum of the storage engine's all_durable time
    // and replication's last applied time. On primaries, the last applied time is updated as
    // transactions commit, which is not necessarily in the order they appear in the oplog. Thus
    // the all_durable time is an appropriate value to read at.

    // On secondaries, however, the all_durable time, as computed by the storage engine, can
    // advance before oplog application completes a batch. This is because the all_durable time
    // is only computed correctly if the storage engine is informed of commit timestamps in
    // increasing order. Because oplog application processes a batch of oplog entries out of order,
    // the timestamping requirement is not satisfied. Secondaries, however, only update the last
    // applied time after a batch completes. Thus last applied is a valid no-overlap point on
    // secondaries.

    // By taking the minimum of the two values, storage can compute a legal time to read at without
    // knowledge of the replication state. The no-overlap point is the minimum of the all_durable
    // time, which represents the point where no transactions will commit any earlier, and
    // lastApplied, which represents the highest optime a node has applied, a point no readers
    // should read afterward.
    Timestamp readTimestamp = (lastApplied) ? std::min(*lastApplied, allDurable) : allDurable;

    if (readTimestamp.isNull()) {
        // When there is not an all_durable or lastApplied timestamp available, read without a
        // timestamp. Do not round up the read timestamp to the oldest timestamp.

        // There is a race that allows new transactions to start between the time we check for a
        // read timestamp and start our transaction, which can temporarily violate the contract of
        // kNoOverlap. That is, writes will be visible that occur after the all_durable time. This
        // is only possible for readers that start immediately after an initial sync that did not
        // replicate any oplog entries. Future transactions will start reading at a timestamp once
        // timestamped writes have been made.
        WiredTigerBeginTxnBlock txnOpen(session,
                                        _prepareConflictBehavior,
                                        _roundUpPreparedTimestamps,
                                        RoundUpReadTimestamp::kNoRoundError,
                                        _untimestampedWriteAssertion);
        LOGV2_DEBUG(4452900, 1, "no read timestamp available for kNoOverlap");
        txnOpen.done();
        return readTimestamp;
    }

    WiredTigerBeginTxnBlock txnOpen(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound,
                                    _untimestampedWriteAssertion);
    auto status = txnOpen.setReadSnapshot(readTimestamp);
    fassert(51066, status);

    // We might have rounded to oldest between calling getAllDurable and setReadSnapshot. We
    // need to get the actual read timestamp we used.
    readTimestamp = _getTransactionReadTimestamp(session);
    txnOpen.done();
    return readTimestamp;
}

Timestamp WiredTigerRecoveryUnit::_getTransactionReadTimestamp(WT_SESSION* session) {
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    auto wtstatus = session->query_timestamp(session, buf, "get=read");
    invariantWTOK(wtstatus, session);
    uint64_t read_timestamp;
    fassert(50949, NumberParser().base(16)(buf, &read_timestamp));
    return Timestamp(read_timestamp);
}

void WiredTigerRecoveryUnit::_updateMultiTimestampConstraint(Timestamp timestamp) {
    std::stack<Timestamp>& timestampOrder = _multiTimestampConstraintTracker.timestampOrder;
    if (!timestampOrder.empty() && timestampOrder.top() == timestamp) {
        // We're still on the same timestamp.
        return;
    }

    timestampOrder.push(timestamp);
}

Status WiredTigerRecoveryUnit::setTimestamp(Timestamp timestamp) {
    _ensureSession();
    LOGV2_DEBUG(22415,
                3,
                "WT set timestamp of future write operations to {timestamp}",
                "timestamp"_attr = timestamp);
    WT_SESSION* session = _session->getSession();
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set WUOW timestamp to " << timestamp.toString());
    invariant(_readAtTimestamp.isNull() || timestamp >= _readAtTimestamp,
              str::stream() << "future commit timestamp " << timestamp.toString()
                            << " cannot be older than read timestamp "
                            << _readAtTimestamp.toString());

    _updateMultiTimestampConstraint(timestamp);
    _lastTimestampSet = timestamp;

    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(*this, timestamp);
    }

    // Starts the WT transaction associated with this session.
    getSession();

    if (MONGO_unlikely(doUntimestampedWritesForIdempotencyTests.shouldFail())) {
        _isTimestamped = true;
        return Status::OK();
    }

    auto rc =
        session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_COMMIT, timestamp.asULL());
    if (rc == 0) {
        _isTimestamped = true;
    }
    return wtRCToStatus(rc, session, "timestamp_transaction");
}

void WiredTigerRecoveryUnit::setCommitTimestamp(Timestamp timestamp) {
    // This can be called either outside of a WriteUnitOfWork or in a prepared transaction after
    // setPrepareTimestamp() is called. Prepared transactions ensure the correct timestamping
    // semantics and the set-once commitTimestamp behavior is exactly what prepared transactions
    // want.
    invariant(!_inUnitOfWork() || !_prepareTimestamp.isNull(), toString(_getState()));
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set it to " << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set commit timestamp to " << timestamp.toString());
    invariant(!_isTimestamped);

    _commitTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getCommitTimestamp() const {
    return _commitTimestamp;
}

void WiredTigerRecoveryUnit::setDurableTimestamp(Timestamp timestamp) {
    invariant(
        _durableTimestamp.isNull(),
        str::stream() << "Trying to reset durable timestamp when it was already set. wasSetTo: "
                      << _durableTimestamp.toString() << " setTo: " << timestamp.toString());

    _durableTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getDurableTimestamp() const {
    return _durableTimestamp;
}

void WiredTigerRecoveryUnit::clearCommitTimestamp() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!_commitTimestamp.isNull());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to clear commit timestamp.");
    invariant(!_isTimestamped);

    _commitTimestamp = Timestamp();
}

void WiredTigerRecoveryUnit::setPrepareTimestamp(Timestamp timestamp) {
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(_prepareTimestamp.isNull(),
              str::stream() << "Trying to set prepare timestamp to " << timestamp.toString()
                            << ". It's already set to " << _prepareTimestamp.toString());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp is " << _commitTimestamp.toString()
                            << " and trying to set prepare timestamp to " << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set prepare timestamp to " << timestamp.toString());

    _prepareTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getPrepareTimestamp() const {
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(!_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp is " << _commitTimestamp.toString()
                            << " and trying to get prepare timestamp of "
                            << _prepareTimestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to get prepare timestamp of "
                            << _prepareTimestamp.toString());

    return _prepareTimestamp;
}

void WiredTigerRecoveryUnit::setPrepareConflictBehavior(PrepareConflictBehavior behavior) {
    // If there is an open storage transaction, it is not valid to try to change the behavior of
    // ignoring prepare conflicts, since that behavior is applied when the transaction is opened.
    invariant(
        !_isActive(),
        str::stream() << "Current state: " << toString(_getState())
                      << ". Invalid internal state while setting prepare conflict behavior to: "
                      << static_cast<int>(behavior));

    _prepareConflictBehavior = behavior;
}

PrepareConflictBehavior WiredTigerRecoveryUnit::getPrepareConflictBehavior() const {
    return _prepareConflictBehavior;
}

void WiredTigerRecoveryUnit::setRoundUpPreparedTimestamps(bool value) {
    // This cannot be called after WiredTigerRecoveryUnit::_txnOpen.
    invariant(!_isActive(),
              str::stream() << "Can't change round up prepared timestamps flag "
                            << "when current state is " << toString(_getState()));
    _roundUpPreparedTimestamps =
        (value) ? RoundUpPreparedTimestamps::kRound : RoundUpPreparedTimestamps::kNoRound;
}

void WiredTigerRecoveryUnit::setTimestampReadSource(ReadSource readSource,
                                                    boost::optional<Timestamp> provided) {
    tassert(5863604, "Cannot change ReadSource as it is pinned.", !isReadSourcePinned());

    LOGV2_DEBUG(22416,
                3,
                "setting timestamp read source",
                "readSource"_attr = toString(readSource),
                "provided"_attr = ((provided) ? provided->toString() : "none"));
    invariant(!_isActive() || _timestampReadSource == readSource,
              str::stream() << "Current state: " << toString(_getState())
                            << ". Invalid internal state while setting timestamp read source: "
                            << toString(readSource) << ", provided timestamp: "
                            << (provided ? provided->toString() : "none"));
    invariant(!provided == (readSource != ReadSource::kProvided));
    invariant(!(provided && provided->isNull()));

    _timestampReadSource = readSource;
    if (readSource == kLastApplied) {
        // The lastApplied timestamp is not always available if the system has not accepted writes.
        if (auto lastApplied = _sessionCache->snapshotManager().getLastApplied()) {
            _readAtTimestamp = *lastApplied;
        } else {
            _readAtTimestamp = Timestamp();
        }
    } else {
        _readAtTimestamp = (provided) ? *provided : Timestamp();
    }
}

RecoveryUnit::ReadSource WiredTigerRecoveryUnit::getTimestampReadSource() const {
    return _timestampReadSource;
}

void WiredTigerRecoveryUnit::pinReadSource() {
    LOGV2_DEBUG(5863602, 3, "Pinning read source on WT recovery unit");
    _readSourcePinned = true;
}

void WiredTigerRecoveryUnit::unpinReadSource() {
    LOGV2_DEBUG(5863603, 3, "Unpinning WT recovery unit read source");
    _readSourcePinned = false;
}

bool WiredTigerRecoveryUnit::isReadSourcePinned() const {
    return _readSourcePinned;
}

void WiredTigerRecoveryUnit::beginIdle() {
    // Close all cursors, we don't want to keep any old cached cursors around.
    if (_session) {
        _session->closeAllCursors("");
    }
}

std::shared_ptr<StorageStats> WiredTigerRecoveryUnit::getOperationStatistics() const {
    std::shared_ptr<WiredTigerOperationStats> statsPtr(nullptr);

    if (!_session)
        return statsPtr;

    WT_SESSION* s = _session->getSession();
    invariant(s);

    statsPtr = std::make_shared<WiredTigerOperationStats>();
    statsPtr->fetchStats(s, "statistics:session", "statistics=(fast)");

    return statsPtr;
}

void WiredTigerRecoveryUnit::setCatalogConflictingTimestamp(Timestamp timestamp) {
    // This cannot be called after a storage snapshot is allocated.
    invariant(!_isActive(), toString(_getState()));
    invariant(_timestampReadSource == ReadSource::kNoTimestamp,
              str::stream() << "Illegal to set catalog conflicting timestamp for a read source "
                            << static_cast<int>(_timestampReadSource));
    invariant(_catalogConflictTimestamp.isNull(),
              str::stream() << "Trying to set catalog conflicting timestamp to "
                            << timestamp.toString() << ". It's already set to "
                            << _catalogConflictTimestamp.toString());
    invariant(!timestamp.isNull());

    _catalogConflictTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getCatalogConflictingTimestamp() const {
    return _catalogConflictTimestamp;
}

bool WiredTigerRecoveryUnit::gatherWriteContextForDebugging() const {
    return _gatherWriteContextForDebugging;
}

void WiredTigerRecoveryUnit::storeWriteContextForDebugging(const BSONObj& info) {
    _writeContextForDebugging.push_back(info);
}

}  // namespace mongo
