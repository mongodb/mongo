/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/bgsync.h"

#include "mongo/base/counter.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_pool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/data_replicator_external_state_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/rollback_source_impl.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;

namespace repl {

namespace {
const char kHashFieldName[] = "h";
const int kSleepToAllowBatchingMillis = 2;
const int kSmallBatchLimitBytes = 40000;
const Milliseconds kRollbackOplogSocketTimeout(10 * 60 * 1000);

/**
 * Extends DataReplicatorExternalStateImpl to be member state aware.
 */
class DataReplicatorExternalStateBackgroundSync : public DataReplicatorExternalStateImpl {
public:
    DataReplicatorExternalStateBackgroundSync(
        ReplicationCoordinator* replicationCoordinator,
        ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
        BackgroundSync* bgsync);
    bool shouldStopFetching(const HostAndPort& source,
                            const rpc::ReplSetMetadata& metadata) override;

private:
    BackgroundSync* _bgsync;
};

DataReplicatorExternalStateBackgroundSync::DataReplicatorExternalStateBackgroundSync(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
    BackgroundSync* bgsync)
    : DataReplicatorExternalStateImpl(replicationCoordinator, replicationCoordinatorExternalState),
      _bgsync(bgsync) {}

bool DataReplicatorExternalStateBackgroundSync::shouldStopFetching(
    const HostAndPort& source, const rpc::ReplSetMetadata& metadata) {
    if (_bgsync->shouldStopFetching()) {
        return true;
    }

    return DataReplicatorExternalStateImpl::shouldStopFetching(source, metadata);
}

size_t getSize(const BSONObj& o) {
    // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
    return static_cast<size_t>(o.objsize());
}
}  // namespace

MONGO_FP_DECLARE(pauseRsBgSyncProducer);

// The count of items in the buffer
static Counter64 bufferCountGauge;
static ServerStatusMetricField<Counter64> displayBufferCount("repl.buffer.count",
                                                             &bufferCountGauge);
// The size (bytes) of items in the buffer
static Counter64 bufferSizeGauge;
static ServerStatusMetricField<Counter64> displayBufferSize("repl.buffer.sizeBytes",
                                                            &bufferSizeGauge);
// The max size (bytes) of the buffer. If the buffer does not have a size constraint, this is
// set to 0.
static Counter64 bufferMaxSizeGauge;
static ServerStatusMetricField<Counter64> displayBufferMaxSize("repl.buffer.maxSizeBytes",
                                                               &bufferMaxSizeGauge);


BackgroundSync::BackgroundSync(
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
    std::unique_ptr<OplogBuffer> oplogBuffer)
    : _oplogBuffer(std::move(oplogBuffer)),
      _replCoord(getGlobalReplicationCoordinator()),
      _replicationCoordinatorExternalState(replicationCoordinatorExternalState),
      _syncSourceResolver(_replCoord),
      _lastOpTimeFetched(Timestamp(std::numeric_limits<int>::max(), 0),
                         std::numeric_limits<long long>::max()) {
    // Update "repl.buffer.maxSizeBytes" server status metric to reflect the current oplog buffer's
    // max size.
    bufferMaxSizeGauge.increment(_oplogBuffer->getMaxSize() - bufferMaxSizeGauge.get());
}

void BackgroundSync::startup(OperationContext* txn) {
    _oplogBuffer->startup(txn);

    invariant(!_producerThread);
    _producerThread.reset(new stdx::thread(stdx::bind(&BackgroundSync::_run, this)));
}

void BackgroundSync::shutdown(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // Clear the buffer. This unblocks the OplogFetcher if it is blocked with a full queue, but
    // ensures that it won't add anything. It will also unblock the OpApplier pipeline if it is
    // waiting for an operation to be past the slaveDelay point.
    clearBuffer(txn);
    _stopped = true;

    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }

    _inShutdown = true;
}

void BackgroundSync::join(OperationContext* txn) {
    _producerThread->join();
    _oplogBuffer->shutdown(txn);
}

bool BackgroundSync::inShutdown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _inShutdown_inlock();
}

bool BackgroundSync::_inShutdown_inlock() const {
    return _inShutdown;
}

void BackgroundSync::_run() {
    Client::initThread("rsBackgroundSync");
    AuthorizationSession::get(cc())->grantInternalAuthorization();

    while (!inShutdown()) {
        try {
            _runProducer();
        } catch (const DBException& e) {
            std::string msg(str::stream() << "sync producer problem: " << redact(e));
            error() << msg;
            _replCoord->setMyHeartbeatMessage(msg);
            sleepmillis(100);  // sleep a bit to keep from hammering this thread with temp. errors.
        } catch (const std::exception& e2) {
            // redact(std::exception&) doesn't work
            severe() << "sync producer exception: " << redact(e2.what());
            fassertFailed(28546);
        }
    }
    stop();
}

void BackgroundSync::_signalNoNewDataForApplier(OperationContext* txn) {
    // Signal to consumers that we have entered the stopped state
    // if the signal isn't already in the queue.
    const boost::optional<BSONObj> lastObjectPushed = _oplogBuffer->lastObjectPushed(txn);
    if (!lastObjectPushed || !lastObjectPushed->isEmpty()) {
        const BSONObj sentinelDoc;
        _oplogBuffer->pushEvenIfFull(txn, sentinelDoc);
        bufferCountGauge.increment();
        bufferSizeGauge.increment(sentinelDoc.objsize());
    }
}

void BackgroundSync::_runProducer() {
    const MemberState state = _replCoord->getMemberState();
    // Stop when the state changes to primary.
    //
    // TODO(siyuan) Drain mode should imply we're the primary. Fix this condition and the one below
    // after fixing step-down during drain mode.
    if (!_replCoord->isCatchingUp() &&
        (_replCoord->isWaitingForApplierToDrain() || state.primary())) {
        if (!isStopped()) {
            stop();
        }
        if (_replCoord->isWaitingForApplierToDrain()) {
            auto txn = cc().makeOperationContext();
            _signalNoNewDataForApplier(txn.get());
        }
        sleepsecs(1);
        return;
    }

    // TODO(spencer): Use a condition variable to await loading a config.
    if (state.startup()) {
        // Wait for a config to be loaded
        sleepsecs(1);
        return;
    }

    // We need to wait until initial sync has started.
    if (_replCoord->getMyLastAppliedOpTime().isNull()) {
        sleepsecs(1);
        return;
    }
    // we want to start when we're no longer primary
    // start() also loads _lastOpTimeFetched, which we know is set from the "if"
    auto txn = cc().makeOperationContext();
    if (isStopped()) {
        start(txn.get());
    }

    _produce(txn.get());
}

void BackgroundSync::_produce(OperationContext* txn) {

    while (MONGO_FAIL_POINT(pauseRsBgSyncProducer)) {
        sleepmillis(0);
    }

    // this oplog reader does not do a handshake because we don't want the server it's syncing
    // from to track how far it has synced
    {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        if (_lastOpTimeFetched.isNull()) {
            // then we're initial syncing and we're still waiting for this to be set
            lock.unlock();
            sleepsecs(1);
            // if there is no one to sync from
            return;
        }

        if (!_replCoord->isCatchingUp() &&
            (_replCoord->isWaitingForApplierToDrain() || _replCoord->getMemberState().primary())) {
            return;
        }

        if (_inShutdown_inlock()) {
            return;
        }
    }

    // find a target to sync from the last optime fetched
    OpTime lastOpTimeFetched;
    HostAndPort source;
    {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        lastOpTimeFetched = _lastOpTimeFetched;
        _syncSourceHost = HostAndPort();
    }

    SyncSourceResolverResponse syncSourceResp =
        _syncSourceResolver.findSyncSource(txn, lastOpTimeFetched);

    if (syncSourceResp.syncSourceStatus == ErrorCodes::OplogStartMissing) {
        // All (accessible) sync sources were too stale.
        if (_replCoord->isCatchingUp()) {
            warning() << "Too stale to catch up.";
            log() << "Our newest OpTime : " << lastOpTimeFetched;
            log() << "Earliest OpTime available is " << syncSourceResp.earliestOpTimeSeen
                  << " from " << syncSourceResp.getSyncSource();
            sleepsecs(1);
            return;
        }

        error() << "too stale to catch up -- entering maintenance mode";
        log() << "Our newest OpTime : " << lastOpTimeFetched;
        log() << "Earliest OpTime available is " << syncSourceResp.earliestOpTimeSeen;
        log() << "See http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember";
        auto status = _replCoord->setMaintenanceMode(true);
        if (!status.isOK()) {
            warning() << "Failed to transition into maintenance mode: " << status;
        }
        bool worked = _replCoord->setFollowerMode(MemberState::RS_RECOVERING);
        if (!worked) {
            warning() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                      << ". Current state: " << _replCoord->getMemberState();
        }
        return;
    } else if (syncSourceResp.isOK() && !syncSourceResp.getSyncSource().empty()) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _syncSourceHost = syncSourceResp.getSyncSource();
        source = _syncSourceHost;
    } else {
        if (!syncSourceResp.isOK()) {
            log() << "failed to find sync source, received error "
                  << syncSourceResp.syncSourceStatus.getStatus();
        }
        // No sync source found.
        sleepsecs(1);
        return;
    }

    long long lastHashFetched;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_stopped) {
            return;
        }
        lastOpTimeFetched = _lastOpTimeFetched;
        lastHashFetched = _lastFetchedHash;
        if (!_replCoord->isCatchingUp()) {
            _replCoord->signalUpstreamUpdater();
        }
    }

    // Set the applied point if unset. This is most likely the first time we've established a sync
    // source since stepping down or otherwise clearing the applied point. We need to set this here,
    // before the OplogWriter gets a chance to append to the oplog.
    if (StorageInterface::get(txn)->getAppliedThrough(txn).isNull()) {
        StorageInterface::get(txn)->setAppliedThrough(txn, _replCoord->getMyLastAppliedOpTime());
    }

    // "lastFetched" not used. Already set in _enqueueDocuments.
    Status fetcherReturnStatus = Status::OK();
    DataReplicatorExternalStateBackgroundSync dataReplicatorExternalState(
        _replCoord, _replicationCoordinatorExternalState, this);
    OplogFetcher* oplogFetcher;
    try {
        auto executor = _replicationCoordinatorExternalState->getTaskExecutor();
        auto config = _replCoord->getConfig();
        auto onOplogFetcherShutdownCallbackFn =
            [&fetcherReturnStatus](const Status& status, const OpTimeWithHash& lastFetched) {
                fetcherReturnStatus = status;
            };

        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _oplogFetcher = stdx::make_unique<OplogFetcher>(
            executor,
            OpTimeWithHash(lastHashFetched, lastOpTimeFetched),
            source,
            NamespaceString(rsOplogName),
            config,
            _replicationCoordinatorExternalState->getOplogFetcherMaxFetcherRestarts(),
            &dataReplicatorExternalState,
            stdx::bind(&BackgroundSync::_enqueueDocuments,
                       this,
                       stdx::placeholders::_1,
                       stdx::placeholders::_2,
                       stdx::placeholders::_3),
            onOplogFetcherShutdownCallbackFn);
        oplogFetcher = _oplogFetcher.get();
    } catch (const mongo::DBException& ex) {
        fassertFailedWithStatus(34440, exceptionToStatus());
    }

    LOG(1) << "scheduling fetcher to read remote oplog on " << _syncSourceHost << " starting at "
           << oplogFetcher->getCommandObject_forTest()["filter"];
    auto scheduleStatus = oplogFetcher->startup();
    if (!scheduleStatus.isOK()) {
        warning() << "unable to schedule fetcher to read remote oplog on " << source << ": "
                  << scheduleStatus;
        return;
    }

    oplogFetcher->join();
    LOG(1) << "fetcher stopped reading remote oplog on " << source;

    // If the background sync is stopped after the fetcher is started, we need to
    // re-evaluate our sync source and oplog common point.
    if (isStopped()) {
        return;
    }

    if (fetcherReturnStatus.code() == ErrorCodes::OplogOutOfOrder) {
        // This is bad because it means that our source
        // has not returned oplog entries in ascending ts order, and they need to be.

        warning() << redact(fetcherReturnStatus);
        // Do not blacklist the server here, it will be blacklisted when we try to reuse it,
        // if it can't return a matching oplog start from the last fetch oplog ts field.
        return;
    } else if (fetcherReturnStatus.code() == ErrorCodes::OplogStartMissing ||
               fetcherReturnStatus.code() == ErrorCodes::RemoteOplogStale) {
        if (_replCoord->isCatchingUp()) {
            warning() << "Rollback situation detected in catch-up mode; catch-up mode will end.";
            sleepsecs(1);
            return;
        }

        // Rollback is a synchronous operation that uses the task executor and may not be
        // executed inside the fetcher callback.
        const int messagingPortTags = 0;
        ConnectionPool connectionPool(messagingPortTags);
        std::unique_ptr<ConnectionPool::ConnectionPtr> connection;
        auto getConnection = [&connection, &connectionPool, source]() -> DBClientBase* {
            if (!connection.get()) {
                connection.reset(new ConnectionPool::ConnectionPtr(
                    &connectionPool, source, Date_t::now(), kRollbackOplogSocketTimeout));
            };
            return connection->get();
        };

        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            lastOpTimeFetched = _lastOpTimeFetched;
        }

        log() << "Starting rollback due to " << redact(fetcherReturnStatus);

        // Wait till all buffered oplog entries have drained and been applied.
        auto lastApplied = _replCoord->getMyLastAppliedOpTime();
        if (lastApplied != lastOpTimeFetched) {
            log() << "Waiting for all operations from " << lastApplied << " until "
                  << lastOpTimeFetched << " to be applied before starting rollback.";
            while (lastOpTimeFetched > (lastApplied = _replCoord->getMyLastAppliedOpTime())) {
                sleepmillis(10);
                if (isStopped() || inShutdown()) {
                    return;
                }
            }
        }
        // check that we are at minvalid, otherwise we cannot roll back as we may be in an
        // inconsistent state
        const auto minValid = StorageInterface::get(txn)->getMinValid(txn);
        if (lastApplied < minValid) {
            fassertNoTrace(18750,
                           Status(ErrorCodes::UnrecoverableRollbackError,
                                  str::stream() << "need to rollback, but in inconsistent state. "
                                                << "minvalid: "
                                                << minValid.toString()
                                                << " > our last optime: "
                                                << lastApplied.toString()));
        }

        _rollback(txn, source, getConnection);
        stop();
    } else if (fetcherReturnStatus == ErrorCodes::InvalidBSON) {
        Seconds blacklistDuration(60);
        warning() << "Fetcher got invalid BSON while querying oplog. Blacklisting sync source "
                  << source << " for " << blacklistDuration << ".";
        _replCoord->blacklistSyncSource(source, Date_t::now() + blacklistDuration);
    } else if (!fetcherReturnStatus.isOK()) {
        warning() << "Fetcher stopped querying remote oplog with error: "
                  << redact(fetcherReturnStatus);
    }
}

void BackgroundSync::_enqueueDocuments(Fetcher::Documents::const_iterator begin,
                                       Fetcher::Documents::const_iterator end,
                                       const OplogFetcher::DocumentsInfo& info) {
    // If this is the first batch of operations returned from the query, "toApplyDocumentCount" will
    // be one fewer than "networkDocumentCount" because the first document (which was applied
    // previously) is skipped.
    if (info.toApplyDocumentCount == 0) {
        return;  // Nothing to do.
    }

    auto txn = cc().makeOperationContext();

    // Wait for enough space.
    _oplogBuffer->waitForSpace(txn.get(), info.toApplyDocumentBytes);

    {
        // Don't add more to the buffer if we are in shutdown. Continue holding the lock until we
        // are done to prevent going into shutdown. This avoids a race where shutdown() clears the
        // buffer between the time we check _inShutdown and the point where we finish writing to the
        // buffer.
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        if (_inShutdown) {
            return;
        }

        OCCASIONALLY {
            LOG(2) << "bgsync buffer has " << _oplogBuffer->getSize() << " bytes";
        }

        // Buffer docs for later application.
        _oplogBuffer->pushAllNonBlocking(txn.get(), begin, end);

        // Update last fetched info.
        _lastFetchedHash = info.lastDocument.value;
        _lastOpTimeFetched = info.lastDocument.opTime;
        LOG(3) << "batch resetting _lastOpTimeFetched: " << _lastOpTimeFetched;
    }

    bufferCountGauge.increment(info.toApplyDocumentCount);
    bufferSizeGauge.increment(info.toApplyDocumentBytes);

    // Check some things periodically (whenever we run out of items in the current cursor batch).
    if (info.networkDocumentBytes > 0 && info.networkDocumentBytes < kSmallBatchLimitBytes) {
        // On a very low latency network, if we don't wait a little, we'll be
        // getting ops to write almost one at a time.  This will both be expensive
        // for the upstream server as well as potentially defeating our parallel
        // application of batches on the secondary.
        //
        // The inference here is basically if the batch is really small, we are "caught up".
        sleepmillis(kSleepToAllowBatchingMillis);
    }
}

bool BackgroundSync::peek(OperationContext* txn, BSONObj* op) {
    return _oplogBuffer->peek(txn, op);
}

void BackgroundSync::waitForMore() {
    // Block for one second before timing out.
    _oplogBuffer->waitForData(Seconds(1));
}

void BackgroundSync::consume(OperationContext* txn) {
    // this is just to get the op off the queue, it's been peeked at
    // and queued for application already
    BSONObj op;
    if (_oplogBuffer->tryPop(txn, &op)) {
        bufferCountGauge.decrement(1);
        bufferSizeGauge.decrement(getSize(op));
    } else {
        invariant(inShutdown());
        // This means that shutdown() was called between the consumer's calls to peek() and
        // consume(). shutdown() cleared the buffer so there is nothing for us to consume here.
        // Since our postcondition is already met, it is safe to return successfully.
    }
}

void BackgroundSync::_rollback(OperationContext* txn,
                               const HostAndPort& source,
                               stdx::function<DBClientBase*()> getConnection) {
    // Abort only when syncRollback detects we are in a unrecoverable state.
    // In other cases, we log the message contained in the error status and retry later.
    auto status = syncRollback(txn,
                               OplogInterfaceLocal(txn, rsOplogName),
                               RollbackSourceImpl(getConnection, source, rsOplogName),
                               _replCoord);
    if (status.isOK()) {
        // When the syncTail thread sees there is no new data by adding something to the buffer.
        _signalNoNewDataForApplier(txn);
        // Wait until the buffer is empty.
        // This is an indication that syncTail has removed the sentinal marker from the buffer
        // and reset its local lastAppliedOpTime via the replCoord.
        while (!_oplogBuffer->isEmpty()) {
            sleepmillis(10);
            if (inShutdown()) {
                return;
            }
        }

        // At this point we are about to leave rollback.  Before we do, wait for any writes done
        // as part of rollback to be durable, and then do any necessary checks that we didn't
        // wind up rolling back something illegal.  We must wait for the rollback to be durable
        // so that if we wind up shutting down uncleanly in response to something we rolled back
        // we know that we won't wind up right back in the same situation when we start back up
        // because the rollback wasn't durable.
        txn->recoveryUnit()->waitUntilDurable();

        // If we detected that we rolled back the shardIdentity document as part of this rollback
        // then we must shut down to clear the in-memory ShardingState associated with the
        // shardIdentity document.
        if (ShardIdentityRollbackNotifier::get(txn)->didRollbackHappen()) {
            severe()
                << "shardIdentity document rollback detected.  Shutting down to clear "
                   "in-memory sharding state.  Restarting this process should safely return it "
                   "to a healthy state";
            fassertFailedNoTrace(40276);
        }

        // It is now safe to clear the ROLLBACK state, which may result in the applier thread
        // transitioning to SECONDARY.  This is safe because the applier thread has now reloaded
        // the new rollback minValid from the database.
        if (!_replCoord->setFollowerMode(MemberState::RS_RECOVERING)) {
            warning() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                      << "; expected to be in state " << MemberState(MemberState::RS_ROLLBACK)
                      << " but found self in " << _replCoord->getMemberState();
        }
        return;
    }
    if (ErrorCodes::UnrecoverableRollbackError == status.code()) {
        fassertNoTrace(28723, status);
    }
    warning() << "rollback cannot proceed at this time (retrying later): " << redact(status);
}

HostAndPort BackgroundSync::getSyncTarget() const {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _syncSourceHost;
}

void BackgroundSync::clearSyncTarget() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _syncSourceHost = HostAndPort();
}

void BackgroundSync::cancelFetcher() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }
}

void BackgroundSync::stop() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _stopped = true;
    _syncSourceHost = HostAndPort();
    _lastOpTimeFetched = OpTime();
    _lastFetchedHash = 0;

    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }
}

void BackgroundSync::start(OperationContext* txn) {
    massert(16235, "going to start syncing, but buffer is not empty", _oplogBuffer->isEmpty());

    long long lastFetchedHash = _readLastAppliedHash(txn);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stopped = false;

    // reset _last fields with current oplog data
    _lastOpTimeFetched = _replCoord->getMyLastAppliedOpTime();
    _lastFetchedHash = lastFetchedHash;

    LOG(1) << "bgsync fetch queue set to: " << _lastOpTimeFetched << " " << _lastFetchedHash;
}

bool BackgroundSync::isStopped() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _stopped;
}

void BackgroundSync::clearBuffer(OperationContext* txn) {
    _oplogBuffer->clear(txn);
    const auto count = bufferCountGauge.get();
    bufferCountGauge.decrement(count);
    const auto size = bufferSizeGauge.get();
    bufferSizeGauge.decrement(size);
}

long long BackgroundSync::_readLastAppliedHash(OperationContext* txn) {
    BSONObj oplogEntry;
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock lk(txn->lockState(), "local", MODE_X);
            bool success = Helpers::getLast(txn, rsOplogName.c_str(), oplogEntry);
            if (!success) {
                // This can happen when we are to do an initial sync.  lastHash will be set
                // after the initial sync is complete.
                return 0;
            }
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "readLastAppliedHash", rsOplogName);
    } catch (const DBException& ex) {
        severe() << "Problem reading " << rsOplogName << ": " << redact(ex);
        fassertFailed(18904);
    }
    long long hash;
    auto status = bsonExtractIntegerField(oplogEntry, kHashFieldName, &hash);
    if (!status.isOK()) {
        severe() << "Most recent entry in " << rsOplogName << " is missing or has invalid \""
                 << kHashFieldName << "\" field. Oplog entry: " << redact(oplogEntry) << ": "
                 << redact(status);
        fassertFailed(18902);
    }
    return hash;
}

bool BackgroundSync::shouldStopFetching() const {
    if (inShutdown()) {
        LOG(2) << "Interrupted by shutdown while checking sync source.";
        return true;
    }

    // If we are transitioning to primary state, we need to stop fetching in order to go into
    // bgsync-stop mode.
    if (_replCoord->isWaitingForApplierToDrain()) {
        LOG(2) << "Interrupted by waiting for applier to drain while checking sync source.";
        return true;
    }

    if (_replCoord->getMemberState().primary() && !_replCoord->isCatchingUp()) {
        LOG(2) << "Interrupted by becoming primary while checking sync source.";
        return true;
    }

    // Check if we have been stopped.
    if (isStopped()) {
        LOG(2) << "Interrupted by a stop request while checking sync source.";
        return true;
    }

    // Check current sync target.
    if (getSyncTarget().empty()) {
        LOG(1) << "Canceling oplog query because we have no valid sync source.";
        return true;
    }

    return false;
}

void BackgroundSync::pushTestOpToBuffer(OperationContext* txn, const BSONObj& op) {
    _oplogBuffer->push(txn, op);
    bufferCountGauge.increment();
    bufferSizeGauge.increment(op.objsize());
}

}  // namespace repl
}  // namespace mongo
