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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Always notifies prepare conflict waiters when a transaction commits or aborts, even when the
// transaction is not prepared. This should always be enabled if WTPrepareConflictForReads is
// used, which fails randomly. If this is not enabled, no prepare conflicts will be resolved,
// because the recovery unit may not ever actually be in a prepared state.
MONGO_FAIL_POINT_DEFINE(WTAlwaysNotifyPrepareConflictWaiters);

// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicWord<unsigned long long> nextSnapshotId{1};

logger::LogSeverity kSlowTransactionSeverity = logger::LogSeverity::Debug(1);

}  // namespace

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
    invariantWTOK(c->reset(c));
}

BSONObj WiredTigerOperationStats::toBSON() {
    BSONObjBuilder bob;
    std::unique_ptr<BSONObjBuilder> dataSection;
    std::unique_ptr<BSONObjBuilder> waitSection;

    for (auto const& stat : _stats) {
        // Find the user consumable name for this statistic.
        auto statIt = _statNameMap.find(stat.first);
        invariant(statIt != _statNameMap.end());

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
    : _sessionCache(sc),
      _oplogManager(oplogManager),
      _mySnapshotId(nextSnapshotId.fetchAndAdd(1)) {}

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

    if (MONGO_FAIL_POINT(WTAlwaysNotifyPrepareConflictWaiters)) {
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

    if (notifyDone || MONGO_FAIL_POINT(WTAlwaysNotifyPrepareConflictWaiters)) {
        _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
    }

    abortRegisteredChanges();
    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
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

    LOG(1) << "preparing transaction at time: " << _prepareTimestamp;

    const std::string conf = "prepare_timestamp=" + integerToHex(_prepareTimestamp.asULL());
    // Prepare the transaction.
    invariantWTOK(s->prepare_transaction(s, conf.c_str()));
}

void WiredTigerRecoveryUnit::commitUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    _commit();
}

void WiredTigerRecoveryUnit::abortUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    _abort();
}

void WiredTigerRecoveryUnit::_ensureSession() {
    if (!_session) {
        _session = _sessionCache->getSession();
    }
}

bool WiredTigerRecoveryUnit::waitUntilDurable() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    const bool forceCheckpoint = false;
    const bool stableCheckpoint = false;
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
    return true;
}

bool WiredTigerRecoveryUnit::waitUntilUnjournaledWritesDurable(bool stableCheckpoint) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    const bool forceCheckpoint = true;
    // Calling `waitUntilDurable` with `forceCheckpoint` set to false only performs a log
    // (journal) flush, and thus has no effect on unjournaled writes. Setting `forceCheckpoint` to
    // true will lock in stable writes to unjournaled tables.
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
    return true;
}

void WiredTigerRecoveryUnit::assertInActiveTxn() const {
    if (_isActive()) {
        return;
    }
    severe() << "Recovery unit is not active. Current state: " << toString(_getState());
    fassertFailed(28575);
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

void WiredTigerRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    if (_isActive()) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::preallocateSnapshot() {
    // Begin a new transaction, if one is not already started.
    getSession();
}

void WiredTigerRecoveryUnit::_txnClose(bool commit) {
    invariant(_isActive(), toString(_getState()));
    WT_SESSION* s = _session->getSession();
    if (_timer) {
        const int transactionTime = _timer->millis();
        // `serverGlobalParams.slowMs` can be set to values <= 0. In those cases, give logging a
        // break.
        if (transactionTime >= std::max(1, serverGlobalParams.slowMS)) {
            LOG(kSlowTransactionSeverity) << "Slow WT transaction. Lifetime of SnapshotId "
                                          << _mySnapshotId << " was " << transactionTime << "ms";
        }
    }

    int wtRet;
    if (commit) {
        StringBuilder conf;
        if (!_commitTimestamp.isNull()) {
            // There is currently no scenario where it is intentional to commit before the current
            // read timestamp.
            invariant(_readAtTimestamp.isNull() || _commitTimestamp >= _readAtTimestamp);

            conf << "commit_timestamp=" << integerToHex(_commitTimestamp.asULL()) << ",";
            _isTimestamped = true;
        }

        if (!_durableTimestamp.isNull()) {
            conf << "durable_timestamp=" << integerToHex(_durableTimestamp.asULL());
        }

        if (_mustBeTimestamped) {
            invariant(_isTimestamped);
        }

        wtRet = s->commit_transaction(s, conf.str().c_str());
        LOG(3) << "WT commit_transaction for snapshot id " << _mySnapshotId;
    } else {
        wtRet = s->rollback_transaction(s, nullptr);
        invariant(!wtRet);
        LOG(3) << "WT rollback_transaction for snapshot id " << _mySnapshotId;
    }

    if (_isTimestamped) {
        if (!_orderedCommit) {
            // We only need to update oplog visibility where commits can be out-of-order with
            // respect to their assigned optime and such commits might otherwise be visible.
            // This should happen only on primary nodes.
            _oplogManager->triggerJournalFlush();
        }
        _isTimestamped = false;
    }
    invariantWTOK(wtRet);

    invariant(!_lastTimestampSet || _commitTimestamp.isNull(),
              str::stream() << "Cannot have both a _lastTimestampSet and a "
                               "_commitTimestamp. _lastTimestampSet: "
                            << _lastTimestampSet->toString()
                            << ". _commitTimestamp: " << _commitTimestamp.toString());

    // We reset the _lastTimestampSet between transactions. Since it is legal for one
    // transaction on a RecoveryUnit to call setTimestamp() and another to call
    // setCommitTimestamp().
    _lastTimestampSet = boost::none;

    _prepareTimestamp = Timestamp();
    _durableTimestamp = Timestamp();
    _roundUpPreparedTimestamps = RoundUpPreparedTimestamps::kNoRound;
    _mySnapshotId = nextSnapshotId.fetchAndAdd(1);
    _isOplogReader = false;
    _oplogVisibleTs = boost::none;
    _orderedCommit = true;  // Default value is true; we assume all writes are ordered.
    _mustBeTimestamped = false;
}

SnapshotId WiredTigerRecoveryUnit::getSnapshotId() const {
    // TODO: use actual wiredtiger txn id
    return SnapshotId(_mySnapshotId);
}

Status WiredTigerRecoveryUnit::obtainMajorityCommittedSnapshot() {
    invariant(_timestampReadSource == ReadSource::kMajorityCommitted);
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }
    _majorityCommittedSnapshot = *snapshotName;
    return Status::OK();
}

boost::optional<Timestamp> WiredTigerRecoveryUnit::getPointInTimeReadTimestamp() {
    // After a ReadSource has been set on this RecoveryUnit, callers expect that this method returns
    // the read timestamp that will be used for current or future transactions. Because callers use
    // this timestamp to inform visiblity of operations, it is therefore necessary to open a
    // transaction to establish a read timestamp, but only for ReadSources that are expected to have
    // read timestamps.
    switch (_timestampReadSource) {
        case ReadSource::kUnset:
        case ReadSource::kNoTimestamp:
        case ReadSource::kCheckpoint:
            return boost::none;
        case ReadSource::kMajorityCommitted:
            // This ReadSource depends on a previous call to obtainMajorityCommittedSnapshot() and
            // does not require an open transaction to return a valid timestamp.
            invariant(!_majorityCommittedSnapshot.isNull());
            return _majorityCommittedSnapshot;
        case ReadSource::kProvided:
            // The read timestamp is set by the user and does not require a transaction to be open.
            invariant(!_readAtTimestamp.isNull());
            return _readAtTimestamp;

        // The following ReadSources can only establish a read timestamp when a transaction is
        // opened.
        case ReadSource::kNoOverlap:
        case ReadSource::kLastApplied:
        case ReadSource::kAllDurableSnapshot:
            break;
    }

    // Ensure a transaction is opened.
    getSession();

    switch (_timestampReadSource) {
        case ReadSource::kLastApplied:
            // The lastApplied timestamp is not always available, so it is not possible to invariant
            // that it exists as other ReadSources do.
            if (!_readAtTimestamp.isNull()) {
                return _readAtTimestamp;
            }
            return boost::none;
        case ReadSource::kNoOverlap:
        case ReadSource::kAllDurableSnapshot:
            invariant(!_readAtTimestamp.isNull());
            return _readAtTimestamp;

        // The follow ReadSources returned values in the first switch block.
        case ReadSource::kUnset:
        case ReadSource::kNoTimestamp:
        case ReadSource::kMajorityCommitted:
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
    if (shouldLog(kSlowTransactionSeverity)) {
        _timer.reset(new Timer());
    }
    WT_SESSION* session = _session->getSession();

    switch (_timestampReadSource) {
        case ReadSource::kUnset:
        case ReadSource::kNoTimestamp: {
            if (_isOplogReader) {
                _oplogVisibleTs = static_cast<std::int64_t>(_oplogManager->getOplogReadTimestamp());
            }
            WiredTigerBeginTxnBlock(session, _prepareConflictBehavior, _roundUpPreparedTimestamps)
                .done();
            break;
        }
        case ReadSource::kCheckpoint: {
            WiredTigerBeginTxnBlock(session, _prepareConflictBehavior, _roundUpPreparedTimestamps)
                .done();
            break;
        }
        case ReadSource::kMajorityCommitted: {
            // We reset _majorityCommittedSnapshot to the actual read timestamp used when the
            // transaction was started.
            _majorityCommittedSnapshot =
                _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(
                    session, _prepareConflictBehavior, _roundUpPreparedTimestamps);
            break;
        }
        case ReadSource::kLastApplied: {
            if (_sessionCache->snapshotManager().getLocalSnapshot()) {
                _readAtTimestamp = _sessionCache->snapshotManager().beginTransactionOnLocalSnapshot(
                    session, _prepareConflictBehavior, _roundUpPreparedTimestamps);
            } else {
                WiredTigerBeginTxnBlock(
                    session, _prepareConflictBehavior, _roundUpPreparedTimestamps)
                    .done();
            }
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
            // Intentionally continue to the next case to read at the _readAtTimestamp.
        }
        case ReadSource::kProvided: {
            WiredTigerBeginTxnBlock txnOpen(
                session, _prepareConflictBehavior, _roundUpPreparedTimestamps);
            auto status = txnOpen.setReadSnapshot(_readAtTimestamp);

            if (!status.isOK() && status.code() == ErrorCodes::BadValue) {
                uasserted(ErrorCodes::SnapshotTooOld,
                          str::stream() << "Read timestamp " << _readAtTimestamp.toString()
                                        << " is older than the oldest available timestamp.");
            }
            uassertStatusOK(status);
            txnOpen.done();
            break;
        }
    }

    LOG(3) << "WT begin_transaction for snapshot id " << _mySnapshotId;
}

Timestamp WiredTigerRecoveryUnit::_beginTransactionAtAllDurableTimestamp(WT_SESSION* session) {
    WiredTigerBeginTxnBlock txnOpen(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound);
    Timestamp txnTimestamp = Timestamp(_oplogManager->fetchAllDurableValue(session->connection));
    auto status = txnOpen.setReadSnapshot(txnTimestamp);
    fassert(50948, status);

    // Since this is not in a critical section, we might have rounded to oldest between
    // calling getAllDurable and setReadSnapshot.  We need to get the actual read timestamp we
    // used.
    auto readTimestamp = _getTransactionReadTimestamp(session);
    txnOpen.done();
    return readTimestamp;
}

Timestamp WiredTigerRecoveryUnit::_beginTransactionAtNoOverlapTimestamp(WT_SESSION* session) {

    auto lastApplied = _sessionCache->snapshotManager().getLocalSnapshot();
    Timestamp allDurable = Timestamp(_oplogManager->fetchAllDurableValue(session->connection));

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

    WiredTigerBeginTxnBlock txnOpen(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound);
    auto status = txnOpen.setReadSnapshot(readTimestamp);
    fassert(51066, status);

    // We might have rounded to oldest between calling getAllDurable and setReadSnapshot. We need
    // to get the actual read timestamp we used.
    readTimestamp = _getTransactionReadTimestamp(session);
    txnOpen.done();
    return readTimestamp;
}

Timestamp WiredTigerRecoveryUnit::_getTransactionReadTimestamp(WT_SESSION* session) {
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    auto wtstatus = session->query_timestamp(session, buf, "get=read");
    invariantWTOK(wtstatus);
    uint64_t read_timestamp;
    fassert(50949, NumberParser().base(16)(buf, &read_timestamp));
    return Timestamp(read_timestamp);
}

Status WiredTigerRecoveryUnit::setTimestamp(Timestamp timestamp) {
    _ensureSession();
    LOG(3) << "WT set timestamp of future write operations to " << timestamp;
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

    _lastTimestampSet = timestamp;

    // Starts the WT transaction associated with this session.
    getSession();

    const std::string conf = "commit_timestamp=" + integerToHex(timestamp.asULL());
    auto rc = session->timestamp_transaction(session, conf.c_str());
    if (rc == 0) {
        _isTimestamped = true;
    }
    return wtRCToStatus(rc, "timestamp_transaction");
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
    LOG(3) << "setting timestamp read source: " << static_cast<int>(readSource)
           << ", provided timestamp: " << ((provided) ? provided->toString() : "none");

    invariant(!_isActive() || _timestampReadSource == readSource,
              str::stream() << "Current state: " << toString(_getState())
                            << ". Invalid internal state while setting timestamp read source: "
                            << static_cast<int>(readSource) << ", provided timestamp: "
                            << (provided ? provided->toString() : "none"));
    invariant(!provided == (readSource != ReadSource::kProvided));
    invariant(!(provided && provided->isNull()));

    _timestampReadSource = readSource;
    _readAtTimestamp = (provided) ? *provided : Timestamp();
}

RecoveryUnit::ReadSource WiredTigerRecoveryUnit::getTimestampReadSource() const {
    return _timestampReadSource;
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

}  // namespace mongo
