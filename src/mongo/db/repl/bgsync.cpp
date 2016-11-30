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
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/rpc/get_status_from_command_result.h"
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
    std::unique_ptr<OplogBuffer> oplogBuffer)
    : _oplogBuffer(std::move(oplogBuffer)),
      _replCoord(getGlobalReplicationCoordinator()),
      _replicationCoordinatorExternalState(replicationCoordinatorExternalState) {
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
    _state = ProducerState::Stopped;

    if (_syncSourceResolver) {
        _syncSourceResolver->shutdown();
    }

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
    auto txn = cc().makeOperationContext();
    if (getState() == ProducerState::Starting) {
        start(txn.get());
    }

    _produce(txn.get());
}

void BackgroundSync::_produce(OperationContext* txn) {
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
    SyncSourceResolverResponse syncSourceResp;
    {
        const OpTime minValidSaved = StorageInterface::get(txn)->getMinValid(txn);

        stdx::lock_guard<stdx::mutex> lock(_mutex);
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
        // TODO: End catchup mode early if we are too stale.
        if (_replCoord->getMemberState().primary()) {
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
    if (StorageInterface::get(txn)->getAppliedThrough(txn).isNull()) {
        StorageInterface::get(txn)->setAppliedThrough(txn, _replCoord->getMyLastAppliedOpTime());
    }

    // "lastFetched" not used. Already set in _enqueueDocuments.
    Status fetcherReturnStatus = Status::OK();
    DataReplicatorExternalStateBackgroundSync dataReplicatorExternalState(
        _replCoord, _replicationCoordinatorExternalState, this);
    auto rbidCopyForFetcher = syncSourceResp.rbid;  // OplogFetcher's callback modifies this.
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
                       stdx::placeholders::_3,
                       &rbidCopyForFetcher),
            onOplogFetcherShutdownCallbackFn);
        oplogFetcher = _oplogFetcher.get();
    } catch (const mongo::DBException& ex) {
        fassertFailedWithStatus(34440, exceptionToStatus());
    }

    const auto logLevel = Command::testCommandsEnabled ? 0 : 1;
    LOG(logLevel) << "scheduling fetcher to read remote oplog on " << _syncSourceHost
                  << " starting at " << oplogFetcher->getCommandObject_forTest()["filter"];
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
    } else if (fetcherReturnStatus.code() == ErrorCodes::OplogStartMissing ||
               fetcherReturnStatus.code() == ErrorCodes::RemoteOplogStale) {
        if (_replCoord->getMemberState().primary()) {
            // TODO: Abort catchup mode early if rollback detected.
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

        // TODO: change this to call into the Applier directly to block until the applier is
        // drained.
        //
        // Wait till all buffered oplog entries have drained and been applied.
        auto lastApplied = _replCoord->getMyLastAppliedOpTime();
        if (lastApplied != lastOpTimeFetched) {
            log() << "Waiting for all operations from " << lastApplied << " until "
                  << lastOpTimeFetched << " to be applied before starting rollback.";
            while (lastOpTimeFetched > (lastApplied = _replCoord->getMyLastAppliedOpTime())) {
                sleepmillis(10);
                if (getState() != ProducerState::Running) {
                    return;
                }
            }
        }

        _rollback(txn, source, syncSourceResp.rbid, getConnection);
        // Reset the producer to clear the sync source and the last optime fetched.
        stop(true);
        startProducerIfStopped();
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
                                         const OplogFetcher::DocumentsInfo& info,
                                         boost::optional<int>* requiredRBID) {
    // Once we establish our cursor, we need to ensure that our upstream node hasn't rolled back
    // since that could cause it to not have our required minValid point. The cursor will be killed
    // if the upstream node rolls back so we don't need to keep checking. This must be blocking
    // since the Fetcher doesn't give us a way to defer sending the getmores after we return.
    if (*requiredRBID) {
        auto rbidStatus = Status(ErrorCodes::InternalError, "");
        auto handle =
            _replicationCoordinatorExternalState->getTaskExecutor()->scheduleRemoteCommand(
                {getSyncTarget(), "admin", BSON("replSetGetRBID" << 1), nullptr},
                [&](const executor::TaskExecutor::RemoteCommandCallbackArgs& rbidReply) {
                    rbidStatus = rbidReply.response.status;
                    if (!rbidStatus.isOK())
                        return;

                    rbidStatus = getStatusFromCommandResult(rbidReply.response.data);
                    if (!rbidStatus.isOK())
                        return;

                    const auto rbidElem = rbidReply.response.data["rbid"];
                    if (rbidElem.type() != NumberInt) {
                        rbidStatus = Status(ErrorCodes::BadValue,
                                            str::stream() << "Upstream node returned an "
                                                          << "rbid with invalid type "
                                                          << rbidElem.type());
                        return;
                    }
                    if (rbidElem.Int() != **requiredRBID) {
                        rbidStatus = Status(ErrorCodes::BadValue,
                                            "Upstream node rolled back after verifying "
                                            "that it had our MinValid point. Retrying.");
                    }
                });
        if (!handle.isOK())
            return handle.getStatus();

        _replicationCoordinatorExternalState->getTaskExecutor()->wait(handle.getValue());
        if (!rbidStatus.isOK())
            return rbidStatus;

        requiredRBID->reset();  // Don't come back to this block while on this cursor.
    }

    // If this is the first batch of operations returned from the query, "toApplyDocumentCount" will
    // be one fewer than "networkDocumentCount" because the first document (which was applied
    // previously) is skipped.
    if (info.toApplyDocumentCount == 0) {
        return Status::OK();  // Nothing to do.
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
        if (_state != ProducerState::Running) {
            return Status::OK();
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

    return Status::OK();
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
                               boost::optional<int> requiredRBID,
                               stdx::function<DBClientBase*()> getConnection) {
    if (MONGO_FAIL_POINT(rollbackHangBeforeStart)) {
        // This log output is used in js tests so please leave it.
        log() << "rollback - rollbackHangBeforeStart fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(rollbackHangBeforeStart) && !inShutdown()) {
            mongo::sleepsecs(1);
        }
    }

    // Set state to ROLLBACK while we are in this function. This prevents serving reads, even from
    // the oplog. This can fail if we are elected PRIMARY, in which case we better not do any
    // rolling back. If we successfully enter ROLLBACK we will only exit this function fatally or
    // after transitioning to RECOVERING. We always transition to RECOVERING regardless of success
    // or (recoverable) failure since we may be in an inconsistent state. If rollback failed before
    // writing anything, SyncTail will quickly take us to SECONDARY since are are still at our
    // original MinValid, which is fine because we may choose a sync source that doesn't require
    // rollback. If it failed after we wrote to MinValid, then we will pick a sync source that will
    // cause us to roll back to the same common point, which is fine. If we succeeded, we will be
    // consistent as soon as we apply up to/through MinValid and SyncTail will make us SECONDARY
    // then.
    {
        log() << "rollback 0";
        Lock::GlobalWrite globalWrite(txn->lockState());
        if (!_replCoord->setFollowerMode(MemberState::RS_ROLLBACK)) {
            log() << "Cannot transition from " << _replCoord->getMemberState().toString() << " to "
                  << MemberState(MemberState::RS_ROLLBACK).toString();
            return;
        }
    }

    try {
        auto status = syncRollback(txn,
                                   OplogInterfaceLocal(txn, rsOplogName),
                                   RollbackSourceImpl(getConnection, source, rsOplogName),
                                   requiredRBID,
                                   _replCoord);

        // Abort only when syncRollback detects we are in a unrecoverable state.
        // WARNING: these statuses sometimes have location codes which are lost with uassertStatusOK
        // so we need to check here first.
        if (ErrorCodes::UnrecoverableRollbackError == status.code()) {
            severe() << "Unable to complete rollback. A full resync may be needed: "
                     << redact(status);
            fassertFailedNoTrace(28723);
        }

        // In other cases, we log the message contained in the error status and retry later.
        uassertStatusOK(status);
    } catch (const DBException& ex) {
        // UnrecoverableRollbackError should only come from a returned status which is handled
        // above.
        invariant(ex.getCode() != ErrorCodes::UnrecoverableRollbackError);

        warning() << "rollback cannot complete at this time (retrying later): " << redact(ex)
                  << " appliedThrough=" << _replCoord->getMyLastAppliedOpTime()
                  << " minvalid=" << StorageInterface::get(txn)->getMinValid(txn);

        // Sleep a bit to allow upstream node to coalesce, if that was the cause of the failure. If
        // we failed in a way that will keep failing, but wasn't flagged as a fatal failure, this
        // will also prevent us from hot-looping and putting too much load on upstream nodes.
        sleepsecs(5);  // 5 seconds was chosen as a completely arbitrary amount of time.
    } catch (...) {
        std::terminate();
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
        severe() << "shardIdentity document rollback detected.  Shutting down to clear "
                    "in-memory sharding state.  Restarting this process should safely return it "
                    "to a healthy state";
        fassertFailedNoTrace(40276);
    }

    if (!_replCoord->setFollowerMode(MemberState::RS_RECOVERING)) {
        severe() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                 << "; expected to be in state " << MemberState(MemberState::RS_ROLLBACK)
                 << " but found self in " << _replCoord->getMemberState();
        fassertFailedNoTrace(40364);
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

void BackgroundSync::start(OperationContext* txn) {
    OpTimeWithHash lastAppliedOpTimeWithHash;
    do {
        lastAppliedOpTimeWithHash = _readLastAppliedOpTimeWithHash(txn);
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

void BackgroundSync::clearBuffer(OperationContext* txn) {
    _oplogBuffer->clear(txn);
    const auto count = bufferCountGauge.get();
    bufferCountGauge.decrement(count);
    const auto size = bufferSizeGauge.get();
    bufferSizeGauge.decrement(size);
}

OpTimeWithHash BackgroundSync::_readLastAppliedOpTimeWithHash(OperationContext* txn) {
    BSONObj oplogEntry;
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock lk(txn->lockState(), "local", MODE_X);
            bool success = Helpers::getLast(txn, rsOplogName.c_str(), oplogEntry);
            if (!success) {
                // This can happen when we are to do an initial sync.  lastHash will be set
                // after the initial sync is complete.
                return OpTimeWithHash(0);
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

void BackgroundSync::pushTestOpToBuffer(OperationContext* txn, const BSONObj& op) {
    _oplogBuffer->push(txn, op);
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
