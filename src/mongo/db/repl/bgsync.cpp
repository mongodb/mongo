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


#include "mongo/db/repl/bgsync.h"

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_pool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/data_replicator_external_state_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/oplog_interface_remote.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rollback_source_impl.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shutdown_in_progress_quiesce_info.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

using std::string;

namespace repl {

namespace {
const int kSleepToAllowBatchingMillis = 2;
const int kSmallBatchLimitBytes = 40000;
const Milliseconds kRollbackOplogSocketTimeout(10 * 60 * 1000);

// The number of times a node attempted to choose a node to sync from among the available sync
// source options. This occurs if we re-evaluate our sync source, receive an error from the source,
// or step down.
CounterMetric numSyncSourceSelections("repl.syncSource.numSelections");

// The number of times a node kept it's original sync source after re-evaluating if its current sync
// source was optimal.
CounterMetric numTimesChoseSameSyncSource("repl.syncSource.numTimesChoseSame");

// The number of times a node chose a new sync source after re-evaluating if its current sync source
// was optimal.
CounterMetric numTimesChoseDifferentSyncSource("repl.syncSource.numTimesChoseDifferent");

// The number of times a node could not find a sync source when choosing a node to sync from among
// the available options.
CounterMetric numTimesCouldNotFindSyncSource("repl.syncSource.numTimesCouldNotFind");

/**
 * Extends DataReplicatorExternalStateImpl to be member state aware.
 */
class DataReplicatorExternalStateBackgroundSync : public DataReplicatorExternalStateImpl {
public:
    DataReplicatorExternalStateBackgroundSync(
        ReplicationCoordinator* replicationCoordinator,
        ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
        BackgroundSync* bgsync);
    ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                              const rpc::ReplSetMetadata& replMetadata,
                                              const rpc::OplogQueryMetadata& oqMetadata,
                                              const OpTime& previousOpTimeFetched,
                                              const OpTime& lastOpTimeFetched) const final;

    ChangeSyncSourceAction shouldStopFetchingOnError(const HostAndPort& source,
                                                     const OpTime& lastOpTimeFetched) const final;

private:
    BackgroundSync* _bgsync;
};

DataReplicatorExternalStateBackgroundSync::DataReplicatorExternalStateBackgroundSync(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
    BackgroundSync* bgsync)
    : DataReplicatorExternalStateImpl(replicationCoordinator, replicationCoordinatorExternalState),
      _bgsync(bgsync) {}

ChangeSyncSourceAction DataReplicatorExternalStateBackgroundSync::shouldStopFetching(
    const HostAndPort& source,
    const rpc::ReplSetMetadata& replMetadata,
    const rpc::OplogQueryMetadata& oqMetadata,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) const {
    if (getReplicationCoordinator()->shouldDropSyncSourceAfterShardSplit(
            replMetadata.getReplicaSetId())) {
        // Drop the last batch of message following a change of replica set due to a shard split.
        LOGV2(6493902,
              "Choosing new sync source because we have joined a new replica set following a shard "
              "split.");
        return ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent;
    }

    if (_bgsync->shouldStopFetching()) {
        return ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch;
    }

    return DataReplicatorExternalStateImpl::shouldStopFetching(
        source, replMetadata, oqMetadata, previousOpTimeFetched, lastOpTimeFetched);
}

ChangeSyncSourceAction DataReplicatorExternalStateBackgroundSync::shouldStopFetchingOnError(
    const HostAndPort& source, const OpTime& lastOpTimeFetched) const {
    if (_bgsync->shouldStopFetching()) {
        return ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent;
    }

    return DataReplicatorExternalStateImpl::shouldStopFetchingOnError(source, lastOpTimeFetched);
}

size_t getSize(const BSONObj& o) {
    // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
    return static_cast<size_t>(o.objsize());
}
}  // namespace

// Failpoint which causes rollback to hang before starting.
MONGO_FAIL_POINT_DEFINE(rollbackHangBeforeStart);

// Failpoint to override the time to sleep before retrying sync source selection.
MONGO_FAIL_POINT_DEFINE(forceBgSyncSyncSourceRetryWaitMS);

// Failpoint which causes rollback to hang after completing.
MONGO_FAIL_POINT_DEFINE(bgSyncHangAfterRunRollback);

BackgroundSync::BackgroundSync(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
    ReplicationProcess* replicationProcess,
    OplogApplier* oplogApplier)
    : _oplogApplier(oplogApplier),
      _replCoord(replicationCoordinator),
      _replicationCoordinatorExternalState(replicationCoordinatorExternalState),
      _replicationProcess(replicationProcess) {}

void BackgroundSync::startup(OperationContext* opCtx) {
    invariant(!_producerThread);
    _producerThread.reset(new stdx::thread([this] { _run(); }));
}

void BackgroundSync::shutdown(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lock(_mutex);

    setState(lock, ProducerState::Stopped);

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
}

bool BackgroundSync::inShutdown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _inShutdown_inlock();
}

bool BackgroundSync::tooStale() const {
    return _tooStale.load();
}

bool BackgroundSync::_inShutdown_inlock() const {
    return _inShutdown;
}

void BackgroundSync::_run() {
    Client::initThread("BackgroundSync");
    AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());

    while (!inShutdown()) {
        try {
            _runProducer();
        } catch (const DBException& e) {
            static constexpr char msg[] = "Sync producer problem";
            LOGV2_ERROR(21125, msg, "error"_attr = redact(e));
            _replCoord->setMyHeartbeatMessage(str::stream() << msg << ": " << redact(e));
            sleepmillis(100);  // sleep a bit to keep from hammering this thread with temp. errors.
        } catch (const std::exception& e2) {
            // redact(std::exception&) doesn't work
            LOGV2_FATAL(28546,
                        "sync producer exception: {error}",
                        "Sync producer error",
                        "error"_attr = redact(e2.what()));
        }
    }
    // No need to reset optimes here because we are shutting down.
    stop(false);
}

void BackgroundSync::_runProducer() {
    {
        // This wait keeps us from spinning.  We will re-check the condition in _produce(), so if
        // the state changes after we release the lock, the behavior is still correct.
        stdx::unique_lock<Latch> lk(_mutex);
        _stateCv.wait(lk, [&]() { return _inShutdown || _state != ProducerState::Stopped; });
        if (_inShutdown)
            return;
    }

    auto memberState = _replCoord->getMemberState();
    invariant(!memberState.rollback());
    invariant(!memberState.startup());

    // We need to wait until initial sync has started.
    if (_replCoord->getMyLastAppliedOpTime().isNull()) {
        sleepsecs(1);
        return;
    }
    // we want to start when we're no longer primary
    // start() also loads _lastOpTimeFetched, which we know is set from the "if"
    {
        auto opCtx = cc().makeOperationContext();
        if (getState() == ProducerState::Starting) {
            start(opCtx.get());
        }
    }
    _produce();
}

void BackgroundSync::_produce() {
    if (MONGO_unlikely(stopReplProducer.shouldFail())) {
        // This log output is used in js tests so please leave it.
        LOGV2(21079,
              "bgsync - stopReplProducer fail point "
              "enabled. Blocking until fail point is disabled.");
        mongo::sleepmillis(_getRetrySleepMS());
        return;
    }

    // this oplog reader does not do a handshake because we don't want the server it's syncing
    // from to track how far it has synced
    HostAndPort oldSource;
    OpTime lastOpTimeFetched;
    HostAndPort source;
    SyncSourceResolverResponse syncSourceResp;
    {
        stdx::unique_lock<Latch> lock(_mutex);
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

        oldSource = _syncSourceHost;
    }

    // find a target to sync from the last optime fetched
    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        lastOpTimeFetched = _lastOpTimeFetched;
        if (!_syncSourceHost.empty()) {
            LOGV2(21080,
                  "Clearing sync source {syncSource} to choose a new one.",
                  "Clearing sync source to choose a new one",
                  "syncSource"_attr = _syncSourceHost);
        }
        _syncSourceHost = HostAndPort();
        _syncSourceResolver = std::make_unique<SyncSourceResolver>(
            _replicationCoordinatorExternalState->getTaskExecutor(),
            _replCoord,
            lastOpTimeFetched,
            OpTime(),
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
    fassert(40349, status);
    _syncSourceResolver->join();
    {
        stdx::lock_guard<Latch> lock(_mutex);
        _syncSourceResolver.reset();
    }

    numSyncSourceSelections.increment(1);

    if (syncSourceResp.syncSourceStatus == ErrorCodes::TooStaleToSyncFromSource) {
        // All (accessible) sync sources are too far ahead of us.
        if (_replCoord->getMemberState().primary()) {
            LOGV2_WARNING(21115,
                          "Too stale to catch up",
                          "lastOpTimeFetched"_attr = lastOpTimeFetched,
                          "earliestOpTimeSeen"_attr = syncSourceResp.earliestOpTimeSeen,
                          "syncSource"_attr = syncSourceResp.getSyncSource());
            auto status = _replCoord->abortCatchupIfNeeded(
                ReplicationCoordinator::PrimaryCatchUpConclusionReason::kFailedWithError);
            if (!status.isOK()) {
                LOGV2_DEBUG(21083,
                            1,
                            "Aborting catch-up failed with status: {error}",
                            "Aborting catch-up failed",
                            "error"_attr = status);
            }
            return;
        }

        if (_tooStale.load()) {
            // We had already marked ourselves too stale.
            return;
        }

        // Need to take the RSTL in mode X to transition out of SECONDARY.
        auto opCtx = cc().makeOperationContext();
        ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);

        LOGV2_ERROR(21126,
                    "Too stale to catch up. Entering maintenance mode. See "
                    "http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember",
                    "lastOpTimeFetched"_attr = lastOpTimeFetched,
                    "earliestOpTimeSeen"_attr = syncSourceResp.earliestOpTimeSeen);

        // Activate maintenance mode and transition to RECOVERING.
        auto status = _replCoord->setMaintenanceMode(opCtx.get(), true);
        if (!status.isOK()) {
            LOGV2_WARNING(21116,
                          "Failed to transition into maintenance mode: {error}",
                          "Failed to transition into maintenance mode",
                          "error"_attr = status);
            // Do not mark ourselves too stale on errors so we can try again next time.
            return;
        }
        status = _replCoord->setFollowerMode(MemberState::RS_RECOVERING);
        if (!status.isOK()) {
            LOGV2_WARNING(21117,
                          "Failed to transition into {targetState}. "
                          "Current state: {currentState}. Caused by: {error}",
                          "Failed to perform replica set state transition",
                          "targetState"_attr = MemberState(MemberState::RS_RECOVERING),
                          "currentState"_attr = _replCoord->getMemberState(),
                          "error"_attr = status);
            // Do not mark ourselves too stale on errors so we can try again next time.
            return;
        }
        _tooStale.store(true);
        return;
    } else if (syncSourceResp.isOK() && !syncSourceResp.getSyncSource().empty()) {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            _syncSourceHost = syncSourceResp.getSyncSource();
            source = _syncSourceHost;
        }
        // If our sync source has not changed, it is likely caused by our heartbeat data map being
        // out of date. In that case we sleep for 1 second to reduce the amount we spin waiting
        // for our map to update.
        if (oldSource == source) {
            long long sleepMS = _getRetrySleepMS();
            LOGV2(21087,
                  "Chose same sync source candidate as last time, {syncSource}. Sleeping for "
                  "{sleepDurationMillis}ms to avoid immediately choosing a new sync source for the "
                  "same reason as last time.",
                  "Chose same sync source candidate as last time. Sleeping to avoid immediately "
                  "choosing a new sync source for the same reason as last time",
                  "syncSource"_attr = source,
                  "sleepDurationMillis"_attr = sleepMS);
            numTimesChoseSameSyncSource.increment(1);
            mongo::sleepmillis(sleepMS);
        } else {
            LOGV2(21088,
                  "Changed sync source from {oldSyncSource} to {newSyncSource}",
                  "Changed sync source",
                  "oldSyncSource"_attr =
                      (oldSource.empty() ? std::string("empty") : oldSource.toString()),
                  "newSyncSource"_attr = source);
            numTimesChoseDifferentSyncSource.increment(1);
        }
    } else {
        if (!syncSourceResp.isOK()) {
            LOGV2(21089,
                  "failed to find sync source, received error "
                  "{error}",
                  "Failed to find sync source",
                  "error"_attr = syncSourceResp.syncSourceStatus.getStatus());
        }

        long long sleepMS = _getRetrySleepMS();
        // No sync source found.
        LOGV2_DEBUG(21090,
                    1,
                    "Could not find a sync source. Sleeping for {sleepDurationMillis}ms before "
                    "trying again.",
                    "Could not find a sync source. Sleeping before trying again",
                    "sleepDurationMillis"_attr = sleepMS);
        numTimesCouldNotFindSyncSource.increment(1);
        mongo::sleepmillis(sleepMS);
        return;
    }

    // If we find a good sync source after having gone too stale, disable maintenance mode so we can
    // transition to SECONDARY.
    if (_tooStale.swap(false)) {
        LOGV2(21091,
              "No longer too stale. Able to sync from {syncSource}",
              "No longer too stale. Able to start syncing",
              "syncSource"_attr = source);

        auto opCtx = cc().makeOperationContext();
        auto status = _replCoord->setMaintenanceMode(opCtx.get(), false);
        if (!status.isOK()) {
            LOGV2_WARNING(21118,
                          "Failed to leave maintenance mode: {error}",
                          "Failed to leave maintenance mode",
                          "error"_attr = status);
        }
    }

    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        lastOpTimeFetched = _lastOpTimeFetched;
    }

    if (!_replCoord->getMemberState().primary()) {
        _replCoord->signalUpstreamUpdater();
    }

    // Set the applied point if unset. This is most likely the first time we've established a sync
    // source since stepping down or otherwise clearing the applied point. We need to set this here,
    // before the OplogWriter gets a chance to append to the oplog.
    {
        auto opCtx = cc().makeOperationContext();

        // Check if the producer has been stopped so that we can prevent setting the applied point
        // after step up has already cleared it. We need to acquire the collection lock before the
        // mutex to preserve proper lock ordering.
        AutoGetCollection autoColl(
            opCtx.get(),
            NamespaceString(ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace),
            MODE_IX);
        stdx::lock_guard<Latch> lock(_mutex);

        if (_state != ProducerState::Running) {
            return;
        }
    }

    // "lastFetched" not used. Already set in _enqueueDocuments.
    Status fetcherReturnStatus = Status::OK();
    int syncSourceRBID = syncSourceResp.rbid;

    DataReplicatorExternalStateBackgroundSync dataReplicatorExternalState(
        _replCoord, _replicationCoordinatorExternalState, this);
    OplogFetcher* oplogFetcher;
    try {
        auto onOplogFetcherShutdownCallbackFn = [&fetcherReturnStatus,
                                                 &syncSourceRBID](const Status& status, int rbid) {
            fetcherReturnStatus = status;
            // If the syncSourceResp rbid is uninitialized, syncSourceRBID will be set to the
            // rbid obtained in the oplog fetcher.
            if (syncSourceRBID == ReplicationProcess::kUninitializedRollbackId) {
                syncSourceRBID = rbid;
            }
        };
        // The construction of OplogFetcher has to be outside bgsync mutex, because it calls
        // replication coordinator.
        auto numRestarts =
            _replicationCoordinatorExternalState->getOplogFetcherSteadyStateMaxFetcherRestarts();
        auto oplogFetcherPtr = std::make_unique<OplogFetcher>(
            _replicationCoordinatorExternalState->getTaskExecutor(),
            std::make_unique<OplogFetcher::OplogFetcherRestartDecisionDefault>(numRestarts),
            &dataReplicatorExternalState,
            [this](const auto& a1, const auto& a2, const auto& a3) {
                return this->_enqueueDocuments(a1, a2, a3);
            },
            onOplogFetcherShutdownCallbackFn,
            OplogFetcher::Config(lastOpTimeFetched,
                                 source,
                                 _replCoord->getConfig(),
                                 syncSourceResp.rbid,
                                 bgSyncOplogFetcherBatchSize));
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        _oplogFetcher = std::move(oplogFetcherPtr);
        oplogFetcher = _oplogFetcher.get();
    } catch (const mongo::DBException&) {
        fassertFailedWithStatus(34440, exceptionToStatus());
    }

    const auto logLevel = TestingProctor::instance().isEnabled() ? 0 : 1;
    LOGV2_DEBUG(21092,
                logLevel,
                "scheduling fetcher to read remote oplog on {syncSource} starting at "
                "{lastOpTimeFetched}",
                "Scheduling fetcher to read remote oplog",
                "syncSource"_attr = source,
                "lastOpTimeFetched"_attr = oplogFetcher->getLastOpTimeFetched_forTest());
    auto scheduleStatus = oplogFetcher->startup();
    if (!scheduleStatus.isOK()) {
        LOGV2_WARNING(21119,
                      "unable to schedule fetcher to read remote oplog on {syncSource}: {error}",
                      "Unable to schedule fetcher to read remote oplog",
                      "syncSource"_attr = source,
                      "error"_attr = scheduleStatus);
        return;
    }

    oplogFetcher->join();
    LOGV2_DEBUG(21093,
                1,
                "fetcher stopped reading remote oplog on {syncSource}",
                "Fetcher stopped reading remote oplog",
                "syncSource"_attr = source);

    // If the background sync is stopped after the fetcher is started, we need to
    // re-evaluate our sync source and oplog common point.
    if (getState() != ProducerState::Running) {
        LOGV2(21094,
              "Replication producer stopped after oplog fetcher finished returning a batch from "
              "our sync source. Abandoning this batch of oplog entries and re-evaluating our sync "
              "source");
        return;
    }

    Milliseconds denylistDuration(60000);
    if (fetcherReturnStatus.code() == ErrorCodes::OplogOutOfOrder) {
        // This is bad because it means that our source
        // has not returned oplog entries in ascending ts order, and they need to be.

        LOGV2_WARNING(
            21120, "Oplog fetcher returned error", "error"_attr = redact(fetcherReturnStatus));
        // Do not denylist the server here, it will be denylisted when we try to reuse it,
        // if it can't return a matching oplog start from the last fetch oplog ts field.
        return;
    } else if (fetcherReturnStatus.code() == ErrorCodes::OplogStartMissing) {
        auto opCtx = cc().makeOperationContext();
        auto storageInterface = StorageInterface::get(opCtx.get());
        _runRollback(opCtx.get(), fetcherReturnStatus, source, syncSourceRBID, storageInterface);

        if (bgSyncHangAfterRunRollback.shouldFail()) {
            LOGV2(21095, "bgSyncHangAfterRunRollback failpoint is set");
            while (MONGO_unlikely(bgSyncHangAfterRunRollback.shouldFail()) && !inShutdown()) {
                mongo::sleepmillis(100);
            }
        }
    } else if (fetcherReturnStatus.code() == ErrorCodes::TooStaleToSyncFromSource) {
        LOGV2_WARNING(
            5579700,
            "Oplog fetcher discovered we are too stale to sync from sync source. Denylisting "
            "sync source",
            "syncSource"_attr = source,
            "denylistDuration"_attr = denylistDuration);
        _replCoord->denylistSyncSource(source, Date_t::now() + denylistDuration);
    } else if (fetcherReturnStatus == ErrorCodes::InvalidBSON) {
        LOGV2_WARNING(
            5579701,
            "Oplog fetcher got invalid BSON while querying oplog. Denylisting sync source "
            "{syncSource} for {denylistDuration}.",
            "Oplog fetcher got invalid BSON while querying oplog. Denylisting sync source",
            "syncSource"_attr = source,
            "denylistDuration"_attr = denylistDuration);
        _replCoord->denylistSyncSource(source, Date_t::now() + denylistDuration);
    } else if (fetcherReturnStatus.code() == ErrorCodes::ShutdownInProgress) {
        if (auto quiesceInfo = fetcherReturnStatus.extraInfo<ShutdownInProgressQuiesceInfo>()) {
            denylistDuration = Milliseconds(quiesceInfo->getRemainingQuiesceTimeMillis());
            LOGV2_WARNING(
                5579702,
                "Sync source was in quiesce mode while we were querying its oplog. Denylisting "
                "sync source",
                "syncSource"_attr = source,
                "denylistDuration"_attr = denylistDuration);
            _replCoord->denylistSyncSource(source, Date_t::now() + denylistDuration);
        }
    } else if (!fetcherReturnStatus.isOK()) {
        LOGV2_WARNING(21122,
                      "Oplog fetcher stopped querying remote oplog with error: {error}",
                      "Oplog fetcher stopped querying remote oplog with error",
                      "error"_attr = redact(fetcherReturnStatus));
    }
}

Status BackgroundSync::_enqueueDocuments(OplogFetcher::Documents::const_iterator begin,
                                         OplogFetcher::Documents::const_iterator end,
                                         const OplogFetcher::DocumentsInfo& info) {
    // If this is the first batch of operations returned from the query, "toApplyDocumentCount" will
    // be one fewer than "networkDocumentCount" because the first document (which was applied
    // previously) is skipped.
    if (info.toApplyDocumentCount == 0) {
        return Status::OK();  // Nothing to do.
    }

    auto opCtx = cc().makeOperationContext();

    // Wait for enough space.
    _oplogApplier->waitForSpace(opCtx.get(), info.toApplyDocumentBytes);

    {
        // Don't add more to the buffer if we are in shutdown. Continue holding the lock until we
        // are done to prevent going into shutdown.
        stdx::unique_lock<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return Status::OK();
        }

        // Buffer docs for later application.
        _oplogApplier->enqueue(opCtx.get(), begin, end);

        // Update last fetched info.
        _lastOpTimeFetched = info.lastDocument;
        LOGV2_DEBUG(21096,
                    3,
                    "batch resetting _lastOpTimeFetched: {lastOpTimeFetched}",
                    "Batch resetting _lastOpTimeFetched",
                    "lastOpTimeFetched"_attr = _lastOpTimeFetched);
    }

    // Check some things periodically (whenever we run out of items in the current cursor batch).
    if (!oplogFetcherUsesExhaust && info.networkDocumentBytes > 0 &&
        info.networkDocumentBytes < kSmallBatchLimitBytes) {
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

void BackgroundSync::_runRollback(OperationContext* opCtx,
                                  const Status& fetcherReturnStatus,
                                  const HostAndPort& source,
                                  int requiredRBID,
                                  StorageInterface* storageInterface) {
    if (_replCoord->getMemberState().primary()) {
        LOGV2_WARNING(21123,
                      "Rollback situation detected in catch-up mode. Aborting catch-up mode");
        auto status = _replCoord->abortCatchupIfNeeded(
            ReplicationCoordinator::PrimaryCatchUpConclusionReason::kFailedWithError);
        if (!status.isOK()) {
            LOGV2_DEBUG(21097,
                        1,
                        "Aborting catch-up failed with status: {error}",
                        "Aborting catch-up failed",
                        "error"_attr = status);
        }
        return;
    }

    ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(opCtx->lockState());

    // Ensure future transactions read without a timestamp.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());

    // Rollback is a synchronous operation that uses the task executor and may not be
    // executed inside the fetcher callback.

    OpTime lastOpTimeFetched;
    {
        stdx::lock_guard<Latch> lock(_mutex);
        lastOpTimeFetched = _lastOpTimeFetched;
    }

    LOGV2(21098,
          "Starting rollback due to fetcher error",
          "error"_attr = redact(fetcherReturnStatus),
          "lastCommittedOpTime"_attr = _replCoord->getLastCommittedOpTime());

    // TODO: change this to call into the Applier directly to block until the applier is
    // drained.
    //
    // Wait till all buffered oplog entries have drained and been applied.
    auto lastApplied = _replCoord->getMyLastAppliedOpTime();
    if (lastApplied != lastOpTimeFetched) {
        LOGV2(21100,
              "Waiting for all operations from {lastApplied} until {lastOpTimeFetched} to be "
              "applied before starting rollback.",
              "Waiting for all operations from lastApplied until lastOpTimeFetched to be applied "
              "before starting rollback",
              "lastApplied"_attr = lastApplied,
              "lastOpTimeFetched"_attr = lastOpTimeFetched);
        while (lastOpTimeFetched > (lastApplied = _replCoord->getMyLastAppliedOpTime())) {
            sleepmillis(10);
            if (getState() != ProducerState::Running) {
                return;
            }
        }
    }

    if (MONGO_unlikely(rollbackHangBeforeStart.shouldFail())) {
        // This log output is used in js tests so please leave it.
        LOGV2(21101,
              "rollback - rollbackHangBeforeStart fail point "
              "enabled. Blocking until fail point is disabled.");
        while (MONGO_unlikely(rollbackHangBeforeStart.shouldFail()) && !inShutdown()) {
            mongo::sleepsecs(1);
        }
    }

    OplogInterfaceLocal localOplog(opCtx);

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

    // Because oplog visibility is updated asynchronously, wait until all uncommitted oplog entries
    // are visible before potentially truncating the oplog.
    storageInterface->waitForAllEarlierOplogWritesToBeVisible(opCtx);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    if (!forceRollbackViaRefetch.load() && storageEngine->supportsRecoverToStableTimestamp()) {
        LOGV2(21102, "Rollback using 'recoverToStableTimestamp' method");
        _runRollbackViaRecoverToCheckpoint(
            opCtx, source, &localOplog, storageInterface, getConnection);
    } else {
        LOGV2(21103, "Rollback using the 'rollbackViaRefetch' method");
        _fallBackOnRollbackViaRefetch(opCtx, source, requiredRBID, &localOplog, getConnection);
    }

    {
        // Reset the producer to clear the sync source and the last optime fetched.
        stdx::lock_guard<Latch> lock(_mutex);
        auto oldProducerState = _state;
        _stop(lock, true);
        // Start the producer only if it was already running, because a concurrent stepUp could have
        // caused rollback to fail, so we avoid restarting the producer if we have become primary.
        if (oldProducerState != ProducerState::Stopped) {
            setState(lock, ProducerState::Starting);
        }
    }
}

void BackgroundSync::_runRollbackViaRecoverToCheckpoint(
    OperationContext* opCtx,
    const HostAndPort& source,
    OplogInterface* localOplog,
    StorageInterface* storageInterface,
    OplogInterfaceRemote::GetConnectionFn getConnection) {

    OplogInterfaceRemote remoteOplog(source,
                                     getConnection,
                                     NamespaceString::kRsOplogNamespace.ns(),
                                     rollbackRemoteOplogQueryBatchSize.load());

    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
    }

    _rollback = std::make_unique<RollbackImpl>(
        localOplog, &remoteOplog, storageInterface, _replicationProcess, _replCoord);

    LOGV2(21104,
          "Scheduling rollback (sync source: {syncSource})",
          "Scheduling rollback",
          "syncSource"_attr = source);
    auto status = _rollback->runRollback(opCtx);
    if (status.isOK()) {
        LOGV2(21105, "Rollback successful");
    } else if (status == ErrorCodes::UnrecoverableRollbackError) {
        LOGV2_FATAL_CONTINUE(21128,
                             "Rollback failed with unrecoverable error: {error}",
                             "Rollback failed with unrecoverable error",
                             "error"_attr = status);
        fassertFailedWithStatusNoTrace(50666, status);
    } else {
        LOGV2_WARNING(21124,
                      "Rollback failed with retryable error: {error}",
                      "Rollback failed with retryable error",
                      "error"_attr = status);
    }
}

void BackgroundSync::_fallBackOnRollbackViaRefetch(
    OperationContext* opCtx,
    const HostAndPort& source,
    int requiredRBID,
    OplogInterface* localOplog,
    OplogInterfaceRemote::GetConnectionFn getConnection) {

    RollbackSourceImpl rollbackSource(getConnection,
                                      source,
                                      NamespaceString::kRsOplogNamespace.ns(),
                                      rollbackRemoteOplogQueryBatchSize.load());

    rollback(opCtx, *localOplog, rollbackSource, requiredRBID, _replCoord, _replicationProcess);
}

HostAndPort BackgroundSync::getSyncTarget() const {
    stdx::unique_lock<Latch> lock(_mutex);
    return _syncSourceHost;
}

void BackgroundSync::clearSyncTarget() {
    stdx::unique_lock<Latch> lock(_mutex);
    LOGV2(21106,
          "Resetting sync source to empty, which was {previousSyncSource}",
          "Resetting sync source to empty",
          "previousSyncSource"_attr = _syncSourceHost);
    _syncSourceHost = HostAndPort();
}

void BackgroundSync::_stop(WithLock lock, bool resetLastFetchedOptime) {
    setState(lock, ProducerState::Stopped);
    LOGV2(21107, "Stopping replication producer");

    _syncSourceHost = HostAndPort();
    if (resetLastFetchedOptime) {
        invariant(_oplogApplier->getBuffer()->isEmpty());
        _lastOpTimeFetched = OpTime();
        LOGV2(21108, "Resetting last fetched optimes in bgsync");
    }

    if (_syncSourceResolver) {
        _syncSourceResolver->shutdown();
    }

    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }
}

void BackgroundSync::stop(bool resetLastFetchedOptime) {
    stdx::lock_guard<Latch> lock(_mutex);
    _stop(lock, resetLastFetchedOptime);
}

void BackgroundSync::start(OperationContext* opCtx) {
    OpTime lastAppliedOpTime;
    ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(opCtx->lockState());

    // Ensure future transactions read without a timestamp.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());

    do {
        lastAppliedOpTime = _readLastAppliedOpTime(opCtx);
        stdx::lock_guard<Latch> lk(_mutex);
        // Double check the state after acquiring the mutex.
        if (_state != ProducerState::Starting) {
            return;
        }
        // If a node steps down during drain mode, then the buffer may not be empty at the beginning
        // of secondary state.
        if (!_oplogApplier->getBuffer()->isEmpty()) {
            LOGV2(21109, "Going to start syncing, but buffer is not empty");
        }
        setState(lk, ProducerState::Running);

        // When a node steps down during drain mode, the last fetched optime would be newer than
        // the last applied.
        if (_lastOpTimeFetched <= lastAppliedOpTime) {
            LOGV2_DEBUG(21110,
                        1,
                        "Setting bgsync _lastOpTimeFetched={lastAppliedOpTime}. Previous "
                        "_lastOpTimeFetched: {previousLastOpTimeFetched}",
                        "Setting bgsync _lastOpTimeFetched to lastAppliedOpTime",
                        "lastAppliedOpTime"_attr = lastAppliedOpTime,
                        "previousLastOpTimeFetched"_attr = _lastOpTimeFetched);
            _lastOpTimeFetched = lastAppliedOpTime;
        }
        // Reload the last applied optime from disk if it has been changed.
    } while (lastAppliedOpTime != _replCoord->getMyLastAppliedOpTime());

    LOGV2_DEBUG(21111,
                1,
                "bgsync fetch queue set to: {lastOpTimeFetched}",
                "bgsync fetch queue set to lastOpTimeFetched",
                "lastOpTimeFetched"_attr = _lastOpTimeFetched);
}

OpTime BackgroundSync::_readLastAppliedOpTime(OperationContext* opCtx) {
    BSONObj oplogEntry;
    try {
        bool success = writeConflictRetry(
            opCtx, "readLastAppliedOpTime", NamespaceString::kRsOplogNamespace.ns(), [&] {
                return Helpers::getLast(
                    opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), oplogEntry);
            });

        if (!success) {
            // This can happen when we are to do an initial sync.
            return OpTime();
        }
    } catch (const ExceptionForCat<ErrorCategory::ShutdownError>&) {
        throw;
    } catch (const DBException& ex) {
        LOGV2_FATAL(18904,
                    "Problem reading {namespace}: {error}",
                    "Problem reading from namespace",
                    "namespace"_attr = NamespaceString::kRsOplogNamespace.ns(),
                    "error"_attr = redact(ex));
    }

    OplogEntry parsedEntry(oplogEntry);
    LOGV2_DEBUG(21112,
                1,
                "Successfully read last entry of oplog while starting bgsync: {lastOplogEntry}",
                "Successfully read last entry of oplog while starting bgsync",
                "lastOplogEntry"_attr = redact(oplogEntry));
    return parsedEntry.getOpTime();
}

bool BackgroundSync::shouldStopFetching() const {
    // Check if we have been stopped.
    if (getState() != ProducerState::Running) {
        LOGV2_DEBUG(21113, 2, "Stopping oplog fetcher due to stop request");
        return true;
    }

    // Check current sync source.
    if (getSyncTarget().empty()) {
        LOGV2_DEBUG(21114,
                    1,
                    "Stopping oplog fetcher; canceling oplog query because we have no valid sync "
                    "source");
        return true;
    }

    return false;
}

BackgroundSync::ProducerState BackgroundSync::getState() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _state;
}

void BackgroundSync::setState(WithLock, ProducerState newState) {
    _state = newState;
    _stateCv.notify_one();
}

void BackgroundSync::startProducerIfStopped() {
    stdx::lock_guard<Latch> lock(_mutex);
    // Let producer run if it's already running.
    if (_state == ProducerState::Stopped) {
        setState(lock, ProducerState::Starting);
    }
}

long long BackgroundSync::_getRetrySleepMS() {
    long long sleepMS = 1000;
    forceBgSyncSyncSourceRetryWaitMS.execute(
        [&](const BSONObj& data) { sleepMS = data["sleepMS"].numberInt(); });
    return sleepMS;
}

}  // namespace repl
}  // namespace mongo
