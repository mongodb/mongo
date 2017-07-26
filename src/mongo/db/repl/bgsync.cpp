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
#include "mongo/base/string_data.h"
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
#include "mongo/db/repl/oplog_interface_remote.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rollback_source_impl.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/rs_rollback_no_uuid.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/memory.h"
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
// 16MB max batch size / 12 byte min doc size * 10 (for good measure) = defaultBatchSize to use.
const auto defaultBatchSize = (16 * 1024 * 1024) / 12 * 10;

// Valid arguments to the rollbackMethod server parameter.
constexpr StringData kRollbackViaRefetchNoUUID = "rollbackViaRefetchNoUUID"_sd;
constexpr StringData kRollbackViaRefetch = "rollbackViaRefetch"_sd;
constexpr StringData kRollbackToCheckpoint = "default"_sd;

// Set this to force rollback to use a particular implementation.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(rollbackMethod,
                                      std::string,
                                      kRollbackViaRefetchNoUUID.toString());

// Checks that only the valid strings above can be used as a rollbackMethod
// parameter. Throws an error if an invalid string is passed to the server parameter.
MONGO_INITIALIZER(rollbackMethod)(InitializerContext*) {
    if ((rollbackMethod != kRollbackViaRefetchNoUUID) && (rollbackMethod != kRollbackViaRefetch) &&
        (rollbackMethod != kRollbackToCheckpoint)) {
        return Status(ErrorCodes::BadValue, "Unsupported rollback method: " + rollbackMethod);
    }
    return Status::OK();
}

// The batchSize to use for the find/getMore queries called by the OplogFetcher
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(bgSyncOplogFetcherBatchSize, int, defaultBatchSize);

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
                            const rpc::ReplSetMetadata& replMetadata,
                            boost::optional<rpc::OplogQueryMetadata> oqMetadata) override;

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
    const HostAndPort& source,
    const rpc::ReplSetMetadata& replMetadata,
    boost::optional<rpc::OplogQueryMetadata> oqMetadata) {
    if (_bgsync->shouldStopFetching()) {
        return true;
    }

    return DataReplicatorExternalStateImpl::shouldStopFetching(source, replMetadata, oqMetadata);
}

size_t getSize(const BSONObj& o) {
    // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
    return static_cast<size_t>(o.objsize());
}
}  // namespace

// Failpoint which causes rollback to hang before starting.
MONGO_FP_DECLARE(rollbackHangBeforeStart);

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
    ReplicationProcess* replicationProcess,
    std::unique_ptr<OplogBuffer> oplogBuffer)
    : _oplogBuffer(std::move(oplogBuffer)),
      _replCoord(getGlobalReplicationCoordinator()),
      _replicationCoordinatorExternalState(replicationCoordinatorExternalState),
      _replicationProcess(replicationProcess) {
    // Update "repl.buffer.maxSizeBytes" server status metric to reflect the current oplog buffer's
    // max size.
    bufferMaxSizeGauge.increment(_oplogBuffer->getMaxSize() - bufferMaxSizeGauge.get());
}

void BackgroundSync::startup(OperationContext* opCtx) {
    _oplogBuffer->startup(opCtx);

    invariant(!_producerThread);
    _producerThread.reset(new stdx::thread(stdx::bind(&BackgroundSync::_run, this)));
}

void BackgroundSync::shutdown(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // Clear the buffer. This unblocks the OplogFetcher if it is blocked with a full queue, but
    // ensures that it won't add anything. It will also unblock the OpApplier pipeline if it is
    // waiting for an operation to be past the slaveDelay point.
    clearBuffer(opCtx);
    _state = ProducerState::Stopped;

    if (_syncSourceResolver) {
        _syncSourceResolver->shutdown();
    }

    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }

    if (_rollback) {
        _rollback->shutdown();
    }

    _inShutdown = true;
}

void BackgroundSync::join(OperationContext* opCtx) {
    _producerThread->join();
    _oplogBuffer->shutdown(opCtx);
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
    stop(true);
}

void BackgroundSync::_runProducer() {
    if (getState() == ProducerState::Stopped) {
        sleepsecs(1);
        return;
    }

    // TODO(spencer): Use a condition variable to await loading a config.
    // TODO(siyuan): Control bgsync with producer state.
    if (_replCoord->getMemberState().startup()) {
        // Wait for a config to be loaded
        sleepsecs(1);
        return;
    }

    invariant(!_replCoord->getMemberState().rollback());

    // We need to wait until initial sync has started.
    if (_replCoord->getMyLastAppliedOpTime().isNull()) {
        sleepsecs(1);
        return;
    }
    // we want to start when we're no longer primary
    // start() also loads _lastOpTimeFetched, which we know is set from the "if"
    auto opCtx = cc().makeOperationContext();
    if (getState() == ProducerState::Starting) {
        start(opCtx.get());
    }

    _produce(opCtx.get());
}

void BackgroundSync::_produce(OperationContext* opCtx) {
    if (MONGO_FAIL_POINT(stopReplProducer)) {
        // This log output is used in js tests so please leave it.
        log() << "bgsync - stopReplProducer fail point "
                 "enabled. Blocking until fail point is disabled.";

        // TODO(SERVER-27120): Remove the return statement and uncomment the while loop.
        // Currently we cannot block here or we prevent primaries from being fully elected since
        // we'll never call _signalNoNewDataForApplier.
        //        while (MONGO_FAIL_POINT(stopReplProducer) && !inShutdown()) {
        //            mongo::sleepsecs(1);
        //        }
        mongo::sleepsecs(1);
        return;
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

        if (_state != ProducerState::Running) {
            return;
        }
    }

    // find a target to sync from the last optime fetched
    OpTime lastOpTimeFetched;
    HostAndPort source;
    HostAndPort oldSource = _syncSourceHost;
    SyncSourceResolverResponse syncSourceResp;
    {
        const OpTime minValidSaved =
            _replicationProcess->getConsistencyMarkers()->getMinValid(opCtx);

        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        const auto requiredOpTime = (minValidSaved > _lastOpTimeFetched) ? minValidSaved : OpTime();
        lastOpTimeFetched = _lastOpTimeFetched;
        _syncSourceHost = HostAndPort();
        _syncSourceResolver = stdx::make_unique<SyncSourceResolver>(
            _replicationCoordinatorExternalState->getTaskExecutor(),
            _replCoord,
            lastOpTimeFetched,
            requiredOpTime,
            [&syncSourceResp](const SyncSourceResolverResponse& resp) { syncSourceResp = resp; });
    }
    // This may deadlock if called inside the mutex because SyncSourceResolver::startup() calls
    // ReplicationCoordinator::chooseNewSyncSource(). ReplicationCoordinatorImpl's mutex has to
    // acquired before BackgroundSync's.
    // It is safe to call startup() outside the mutex on this instance of SyncSourceResolver because
    // we do not destroy this instance outside of this function which is only called from a single
    // thread.
    auto status = _syncSourceResolver->startup();
    if (ErrorCodes::CallbackCanceled == status || ErrorCodes::isShutdownError(status.code())) {
        return;
    }
    fassertStatusOK(40349, status);
    _syncSourceResolver->join();
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _syncSourceResolver.reset();
    }

    if (syncSourceResp.syncSourceStatus == ErrorCodes::OplogStartMissing) {
        // All (accessible) sync sources were too stale.
        if (_replCoord->getMemberState().primary()) {
            warning() << "Too stale to catch up.";
            log() << "Our newest OpTime : " << lastOpTimeFetched;
            log() << "Earliest OpTime available is " << syncSourceResp.earliestOpTimeSeen
                  << " from " << syncSourceResp.getSyncSource();
            _replCoord->abortCatchupIfNeeded().transitional_ignore();
            return;
        }

        // We only need to mark ourselves as too stale once.
        if (_tooStale) {
            return;
        }

        // Mark yourself as too stale.
        _tooStale = true;

        error() << "too stale to catch up -- entering maintenance mode";
        log() << "Our newest OpTime : " << lastOpTimeFetched;
        log() << "Earliest OpTime available is " << syncSourceResp.earliestOpTimeSeen;
        log() << "See http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember";

        // Activate maintenance mode and transition to RECOVERING.
        auto status = _replCoord->setMaintenanceMode(true);
        if (!status.isOK()) {
            warning() << "Failed to transition into maintenance mode: " << status;
        }
        status = _replCoord->setFollowerMode(MemberState::RS_RECOVERING);
        if (!status.isOK()) {
            warning() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                      << ". Current state: " << _replCoord->getMemberState() << causedBy(status);
        }
        return;
    } else if (syncSourceResp.isOK() && !syncSourceResp.getSyncSource().empty()) {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            _syncSourceHost = syncSourceResp.getSyncSource();
            source = _syncSourceHost;
        }
        // If our sync source has not changed, it is likely caused by our heartbeat data map being
        // out of date. In that case we sleep for 1 second to reduce the amount we spin waiting
        // for our map to update.
        if (oldSource == source) {
            log() << "Chose same sync source candidate as last time, " << source
                  << ". Sleeping for 1 second to avoid immediately choosing a new sync source for "
                     "the same reason as last time.";
            sleepsecs(1);
        }
    } else {
        if (!syncSourceResp.isOK()) {
            log() << "failed to find sync source, received error "
                  << syncSourceResp.syncSourceStatus.getStatus();
        }
        // No sync source found.
        sleepsecs(1);
        return;
    }

    // If we find a good sync source after having gone too stale, disable maintenance mode so we can
    // transition to SECONDARY.
    if (_tooStale) {

        _tooStale = false;

        log() << "No longer too stale. Able to sync from " << _syncSourceHost;

        auto status = _replCoord->setMaintenanceMode(false);
        if (!status.isOK()) {
            warning() << "Failed to leave maintenance mode: " << status;
        }
    }

    long long lastHashFetched;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        lastOpTimeFetched = _lastOpTimeFetched;
        lastHashFetched = _lastFetchedHash;
    }

    if (!_replCoord->getMemberState().primary()) {
        _replCoord->signalUpstreamUpdater();
    }

    // Set the applied point if unset. This is most likely the first time we've established a sync
    // source since stepping down or otherwise clearing the applied point. We need to set this here,
    // before the OplogWriter gets a chance to append to the oplog.
    if (_replicationProcess->getConsistencyMarkers()->getAppliedThrough(opCtx).isNull()) {
        _replicationProcess->getConsistencyMarkers()->setAppliedThrough(
            opCtx, _replCoord->getMyLastAppliedOpTime());
    }

    // "lastFetched" not used. Already set in _enqueueDocuments.
    Status fetcherReturnStatus = Status::OK();
    DataReplicatorExternalStateBackgroundSync dataReplicatorExternalState(
        _replCoord, _replicationCoordinatorExternalState, this);
    OplogFetcher* oplogFetcher;
    try {
        auto onOplogFetcherShutdownCallbackFn = [&fetcherReturnStatus](const Status& status) {
            fetcherReturnStatus = status;
        };
        // The construction of OplogFetcher has to be outside bgsync mutex, because it calls
        // replication coordinator.
        auto oplogFetcherPtr = stdx::make_unique<OplogFetcher>(
            _replicationCoordinatorExternalState->getTaskExecutor(),
            OpTimeWithHash(lastHashFetched, lastOpTimeFetched),
            source,
            NamespaceString::kRsOplogNamespace,
            _replCoord->getConfig(),
            _replicationCoordinatorExternalState->getOplogFetcherMaxFetcherRestarts(),
            syncSourceResp.rbid,
            true /* requireFresherSyncSource */,
            &dataReplicatorExternalState,
            stdx::bind(&BackgroundSync::_enqueueDocuments,
                       this,
                       stdx::placeholders::_1,
                       stdx::placeholders::_2,
                       stdx::placeholders::_3),
            onOplogFetcherShutdownCallbackFn,
            bgSyncOplogFetcherBatchSize);
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        _oplogFetcher = std::move(oplogFetcherPtr);
        oplogFetcher = _oplogFetcher.get();
    } catch (const mongo::DBException& ex) {
        fassertFailedWithStatus(34440, exceptionToStatus());
    }

    const auto logLevel = Command::testCommandsEnabled ? 0 : 1;
    LOG(logLevel) << "scheduling fetcher to read remote oplog on " << _syncSourceHost
                  << " starting at " << oplogFetcher->getFindQuery_forTest()["filter"];
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
    if (getState() != ProducerState::Running) {
        log() << "Replication producer stopped after oplog fetcher finished returning a batch from "
                 "our sync source.  Abandoning this batch of oplog entries and re-evaluating our "
                 "sync source.";
        return;
    }

    if (fetcherReturnStatus.code() == ErrorCodes::OplogOutOfOrder) {
        // This is bad because it means that our source
        // has not returned oplog entries in ascending ts order, and they need to be.

        warning() << redact(fetcherReturnStatus);
        // Do not blacklist the server here, it will be blacklisted when we try to reuse it,
        // if it can't return a matching oplog start from the last fetch oplog ts field.
        return;
    } else if (fetcherReturnStatus.code() == ErrorCodes::OplogStartMissing) {
        auto storageInterface = StorageInterface::get(opCtx);
        _runRollback(opCtx, fetcherReturnStatus, source, syncSourceResp.rbid, storageInterface);
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

Status BackgroundSync::_enqueueDocuments(Fetcher::Documents::const_iterator begin,
                                         Fetcher::Documents::const_iterator end,
                                         const OplogFetcher::DocumentsInfo& info) {
    // If this is the first batch of operations returned from the query, "toApplyDocumentCount" will
    // be one fewer than "networkDocumentCount" because the first document (which was applied
    // previously) is skipped.
    if (info.toApplyDocumentCount == 0) {
        return Status::OK();  // Nothing to do.
    }

    auto opCtx = cc().makeOperationContext();

    // Wait for enough space.
    _oplogBuffer->waitForSpace(opCtx.get(), info.toApplyDocumentBytes);

    {
        // Don't add more to the buffer if we are in shutdown. Continue holding the lock until we
        // are done to prevent going into shutdown. This avoids a race where shutdown() clears the
        // buffer between the time we check _inShutdown and the point where we finish writing to the
        // buffer.
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        if (_state != ProducerState::Running) {
            return Status::OK();
        }

        OCCASIONALLY {
            LOG(2) << "bgsync buffer has " << _oplogBuffer->getSize() << " bytes";
        }

        // Buffer docs for later application.
        _oplogBuffer->pushAllNonBlocking(opCtx.get(), begin, end);

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

    return Status::OK();
}

bool BackgroundSync::peek(OperationContext* opCtx, BSONObj* op) {
    return _oplogBuffer->peek(opCtx, op);
}

void BackgroundSync::waitForMore() {
    // Block for one second before timing out.
    _oplogBuffer->waitForData(Seconds(1));
}

void BackgroundSync::consume(OperationContext* opCtx) {
    // this is just to get the op off the queue, it's been peeked at
    // and queued for application already
    BSONObj op;
    if (_oplogBuffer->tryPop(opCtx, &op)) {
        bufferCountGauge.decrement(1);
        bufferSizeGauge.decrement(getSize(op));
    } else {
        invariant(inShutdown());
        // This means that shutdown() was called between the consumer's calls to peek() and
        // consume(). shutdown() cleared the buffer so there is nothing for us to consume here.
        // Since our postcondition is already met, it is safe to return successfully.
    }
}

void BackgroundSync::_runRollback(OperationContext* opCtx,
                                  const Status& fetcherReturnStatus,
                                  const HostAndPort& source,
                                  int requiredRBID,
                                  StorageInterface* storageInterface) {
    if (_replCoord->getMemberState().primary()) {
        warning() << "Rollback situation detected in catch-up mode. Aborting catch-up mode.";
        _replCoord->abortCatchupIfNeeded().transitional_ignore();
        return;
    }

    // Rollback is a synchronous operation that uses the task executor and may not be
    // executed inside the fetcher callback.

    OpTime lastOpTimeFetched;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        lastOpTimeFetched = _lastOpTimeFetched;
    }

    log() << "Starting rollback due to " << redact(fetcherReturnStatus);

    // TODO: change this to call into the Applier directly to block until the applier is
    // drained.
    //
    // Wait till all buffered oplog entries have drained and been applied.
    auto lastApplied = _replCoord->getMyLastAppliedOpTime();
    if (lastApplied != lastOpTimeFetched) {
        log() << "Waiting for all operations from " << lastApplied << " until " << lastOpTimeFetched
              << " to be applied before starting rollback.";
        while (lastOpTimeFetched > (lastApplied = _replCoord->getMyLastAppliedOpTime())) {
            sleepmillis(10);
            if (getState() != ProducerState::Running) {
                return;
            }
        }
    }

    if (MONGO_FAIL_POINT(rollbackHangBeforeStart)) {
        // This log output is used in js tests so please leave it.
        log() << "rollback - rollbackHangBeforeStart fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(rollbackHangBeforeStart) && !inShutdown()) {
            mongo::sleepsecs(1);
        }
    }

    OplogInterfaceLocal localOplog(opCtx, NamespaceString::kRsOplogNamespace.ns());

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

    StorageEngine* engine = opCtx->getServiceContext()->getGlobalStorageEngine();
    bool supportsCheckpointRollback = engine->supportsRecoverToStableTimestamp();
    if (supportsCheckpointRollback && rollbackMethod == kRollbackToCheckpoint) {
        // If we are running on a storage engine that supports "roll back to a checkpoint" then
        // we use the "roll back to a checkpoint" algorithm, regardless of the feature
        // compatibility version (FCV). If the user specifies a different rollback method,
        // we will fall back to that.
        log() << "Rollback using 'roll back to a checkpoint' algorithm.";
        _runRollbackViaRecoverToCheckpoint(
            opCtx, source, &localOplog, storageInterface, getConnection);

    } else if (rollbackMethod != kRollbackViaRefetchNoUUID &&
               (serverGlobalParams.featureCompatibility.version.load() ==
                ServerGlobalParams::FeatureCompatibility::Version::k36)) {
        // If the user is in FCV 3.6 and the user did not specify to fall back on "roll back via
        // refetch" without UUID support, then we use "roll back via refetch" with UUIDs.

        if (rollbackMethod == kRollbackToCheckpoint) {
            invariant(!supportsCheckpointRollback);
            log() << "Rollback falling back on 'rollback via refetch' because this storage "
                     "engine does not support 'roll back to a checkpoint'.";
        } else {
            invariant(rollbackMethod == kRollbackViaRefetch);
            log() << "Rollback falling back on 'rollback via refetch' due to startup "
                     "server parameter.";
        }
        _fallBackOnRollbackViaRefetch(
            opCtx, source, requiredRBID, &localOplog, true, getConnection);
    } else {
        if (rollbackMethod == kRollbackToCheckpoint) {
            invariant(!supportsCheckpointRollback);
            invariant(serverGlobalParams.featureCompatibility.version.load() ==
                      ServerGlobalParams::FeatureCompatibility::Version::k34);
            log() << "Rollback falling back on 'rollback via refetch' without UUID support "
                     "because this storage engine does not support 'roll back to a checkpoint' and "
                     "we are in featureCompatibilityVersion 3.4.";
        } else if (rollbackMethod == kRollbackViaRefetch) {
            invariant(serverGlobalParams.featureCompatibility.version.load() ==
                      ServerGlobalParams::FeatureCompatibility::Version::k34);
            log() << "Rollback falling back on 'rollback via refetch' without UUID support. "
                     "'Rollback via refetch' with UUID support is not feature compatible with "
                     "featureCompatabilityVersion 3.4.";
        } else {
            invariant(rollbackMethod == kRollbackViaRefetchNoUUID);
            log() << "Rollback falling back on 'rollback via refetch' without UUID support due to "
                     "startup server parameter.";
        }
        _fallBackOnRollbackViaRefetch(
            opCtx, source, requiredRBID, &localOplog, false, getConnection);
    }

    // Reset the producer to clear the sync source and the last optime fetched.
    stop(true);
    startProducerIfStopped();
}

void BackgroundSync::_runRollbackViaRecoverToCheckpoint(
    OperationContext* opCtx,
    const HostAndPort& source,
    OplogInterface* localOplog,
    StorageInterface* storageInterface,
    OplogInterfaceRemote::GetConnectionFn getConnection) {

    OplogInterfaceRemote remoteOplog(getConnection, NamespaceString::kRsOplogNamespace.ns());

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
    }

    _rollback = stdx::make_unique<RollbackImpl>(localOplog, &remoteOplog, _replCoord);

    log() << "Scheduling rollback (sync source: " << source << ")";
    auto status = _rollback->runRollback(opCtx);
    if (status.isOK()) {
        log() << "Rollback successful.";
    } else {
        warning() << "Rollback failed with error: " << status;
    }
}

void BackgroundSync::_fallBackOnRollbackViaRefetch(
    OperationContext* opCtx,
    const HostAndPort& source,
    int requiredRBID,
    OplogInterface* localOplog,
    bool useUUID,
    OplogInterfaceRemote::GetConnectionFn getConnection) {

    RollbackSourceImpl rollbackSource(
        getConnection, source, NamespaceString::kRsOplogNamespace.ns());

    if (useUUID) {
        rollback(opCtx, *localOplog, rollbackSource, requiredRBID, _replCoord, _replicationProcess);
    } else {
        rollbackNoUUID(
            opCtx, *localOplog, rollbackSource, requiredRBID, _replCoord, _replicationProcess);
    }
}

HostAndPort BackgroundSync::getSyncTarget() const {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _syncSourceHost;
}

void BackgroundSync::clearSyncTarget() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _syncSourceHost = HostAndPort();
}

void BackgroundSync::stop(bool resetLastFetchedOptime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _state = ProducerState::Stopped;
    _syncSourceHost = HostAndPort();
    if (resetLastFetchedOptime) {
        invariant(_oplogBuffer->isEmpty());
        _lastOpTimeFetched = OpTime();
        _lastFetchedHash = 0;
    }

    if (_syncSourceResolver) {
        _syncSourceResolver->shutdown();
    }

    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }
}

void BackgroundSync::start(OperationContext* opCtx) {
    OpTimeWithHash lastAppliedOpTimeWithHash;
    do {
        lastAppliedOpTimeWithHash = _readLastAppliedOpTimeWithHash(opCtx);
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        // Double check the state after acquiring the mutex.
        if (_state != ProducerState::Starting) {
            return;
        }
        // If a node steps down during drain mode, then the buffer may not be empty at the beginning
        // of secondary state.
        if (!_oplogBuffer->isEmpty()) {
            log() << "going to start syncing, but buffer is not empty";
        }
        _state = ProducerState::Running;

        // When a node steps down during drain mode, the last fetched optime would be newer than
        // the last applied.
        if (_lastOpTimeFetched <= lastAppliedOpTimeWithHash.opTime) {
            _lastOpTimeFetched = lastAppliedOpTimeWithHash.opTime;
            _lastFetchedHash = lastAppliedOpTimeWithHash.value;
        }
        // Reload the last applied optime from disk if it has been changed.
    } while (lastAppliedOpTimeWithHash.opTime != _replCoord->getMyLastAppliedOpTime());

    LOG(1) << "bgsync fetch queue set to: " << _lastOpTimeFetched << " " << _lastFetchedHash;
}

void BackgroundSync::clearBuffer(OperationContext* opCtx) {
    _oplogBuffer->clear(opCtx);
    const auto count = bufferCountGauge.get();
    bufferCountGauge.decrement(count);
    const auto size = bufferSizeGauge.get();
    bufferSizeGauge.decrement(size);
}

OpTimeWithHash BackgroundSync::_readLastAppliedOpTimeWithHash(OperationContext* opCtx) {
    BSONObj oplogEntry;
    try {
        bool success = writeConflictRetry(
            opCtx, "readLastAppliedHash", NamespaceString::kRsOplogNamespace.ns(), [&] {
                Lock::DBLock lk(opCtx, "local", MODE_X);
                return Helpers::getLast(
                    opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), oplogEntry);
            });

        if (!success) {
            // This can happen when we are to do an initial sync.  lastHash will be set
            // after the initial sync is complete.
            return OpTimeWithHash(0);
        }
    } catch (const DBException& ex) {
        severe() << "Problem reading " << NamespaceString::kRsOplogNamespace.ns() << ": "
                 << redact(ex);
        fassertFailed(18904);
    }
    long long hash;
    auto status = bsonExtractIntegerField(oplogEntry, kHashFieldName, &hash);
    if (!status.isOK()) {
        severe() << "Most recent entry in " << NamespaceString::kRsOplogNamespace.ns()
                 << " is missing or has invalid \"" << kHashFieldName
                 << "\" field. Oplog entry: " << redact(oplogEntry) << ": " << redact(status);
        fassertFailed(18902);
    }
    OplogEntry parsedEntry(oplogEntry);
    return OpTimeWithHash(hash, parsedEntry.getOpTime());
}

bool BackgroundSync::shouldStopFetching() const {
    // Check if we have been stopped.
    if (getState() != ProducerState::Running) {
        LOG(2) << "Stopping oplog fetcher due to stop request.";
        return true;
    }

    // Check current sync source.
    if (getSyncTarget().empty()) {
        LOG(1) << "Stopping oplog fetcher; canceling oplog query because we have no valid sync "
                  "source.";
        return true;
    }

    return false;
}

void BackgroundSync::pushTestOpToBuffer(OperationContext* opCtx, const BSONObj& op) {
    _oplogBuffer->push(opCtx, op);
    bufferCountGauge.increment();
    bufferSizeGauge.increment(op.objsize());
}

BackgroundSync::ProducerState BackgroundSync::getState() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _state;
}

void BackgroundSync::startProducerIfStopped() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // Let producer run if it's already running.
    if (_state == ProducerState::Stopped) {
        _state = ProducerState::Starting;
    }
}


}  // namespace repl
}  // namespace mongo
