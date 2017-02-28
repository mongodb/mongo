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

#include <memory>

#include "mongo/base/counter.h"
#include "mongo/client/connection_pool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/rollback_source_impl.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;

namespace repl {

namespace {
const char hashFieldName[] = "h";
int SleepToAllowBatchingMillis = 2;
const int BatchIsSmallish = 40000;  // bytes

/**
 * Returns new thread pool for thead pool task executor.
 */
std::unique_ptr<ThreadPool> makeThreadPool() {
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.poolName = "rsBackgroundSync";
    return stdx::make_unique<ThreadPool>(threadPoolOptions);
}

/**
 * Checks the criteria for rolling back.
 * 'getNextOperation' returns the first result of the oplog tailing query.
 * 'lastOpTimeFetched' should be consistent with the predicate in the query.
 * Returns RemoteOplogStale if the oplog query has no results.
 * Returns OplogStartMissing if we cannot find the timestamp of the last fetched operation in
 * the remote oplog.
 */
Status checkRemoteOplogStart(stdx::function<StatusWith<BSONObj>()> getNextOperation,
                             OpTime lastOpTimeFetched,
                             long long lastHashFetched) {
    auto result = getNextOperation();
    if (!result.isOK()) {
        // The GTE query from upstream returns nothing, so we're ahead of the upstream.
        return Status(ErrorCodes::RemoteOplogStale,
                      "we are ahead of the sync source, will try to roll back");
    }
    BSONObj o = result.getValue();
    OpTime opTime = fassertStatusOK(28778, OpTime::parseFromOplogEntry(o));
    long long hash = o["h"].numberLong();
    if (opTime != lastOpTimeFetched || hash != lastHashFetched) {
        return Status(ErrorCodes::OplogStartMissing,
                      str::stream() << "our last op time fetched: " << lastOpTimeFetched.toString()
                                    << ". source's GTE: " << opTime.toString() << " hashes: ("
                                    << lastHashFetched << "/" << hash << ")");
    }
    return Status::OK();
}

}  // namespace

MONGO_FP_DECLARE(stopReplProducer);

BackgroundSync* BackgroundSync::s_instance = 0;
stdx::mutex BackgroundSync::s_mutex;

// The number and time spent reading batches off the network
static TimerStats getmoreReplStats;
static ServerStatusMetricField<TimerStats> displayBatchesRecieved("repl.network.getmores",
                                                                  &getmoreReplStats);
// The oplog entries read via the oplog reader
static Counter64 opsReadStats;
static ServerStatusMetricField<Counter64> displayOpsRead("repl.network.ops", &opsReadStats);
// The bytes read via the oplog reader
static Counter64 networkByteStats;
static ServerStatusMetricField<Counter64> displayBytesRead("repl.network.bytes", &networkByteStats);

// Failpoint which causes rollback to hang before starting.
MONGO_FP_DECLARE(rollbackHangBeforeStart);

// Failpoint which causes the oplog fetcher to hang before the first fetch.
MONGO_FP_DECLARE(fetcherHangBeforeStart);

// The count of items in the buffer
static Counter64 bufferCountGauge;
static ServerStatusMetricField<Counter64> displayBufferCount("repl.buffer.count",
                                                             &bufferCountGauge);
// The size (bytes) of items in the buffer
static Counter64 bufferSizeGauge;
static ServerStatusMetricField<Counter64> displayBufferSize("repl.buffer.sizeBytes",
                                                            &bufferSizeGauge);
// The max size (bytes) of the buffer
static int bufferMaxSizeGauge = 256 * 1024 * 1024;
static ServerStatusMetricField<int> displayBufferMaxSize("repl.buffer.maxSizeBytes",
                                                         &bufferMaxSizeGauge);


BackgroundSyncInterface::~BackgroundSyncInterface() {}

namespace {
size_t getSize(const BSONObj& o) {
    // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
    return static_cast<size_t>(o.objsize());
}
}  // namespace

const NamespaceString BackgroundSync::kLocalOplogNss("local.oplog.rs");

BackgroundSync::BackgroundSync()
    : _buffer(bufferMaxSizeGauge, &getSize),
      _threadPoolTaskExecutor(makeThreadPool(),
                              executor::makeNetworkInterface("NetworkInterfaceASIO-BGSync")),
      _lastOpTimeFetched(Timestamp(std::numeric_limits<int>::max(), 0),
                         std::numeric_limits<long long>::max()),
      _lastFetchedHash(0),
      _stopped(true),
      _replCoord(getGlobalReplicationCoordinator()),
      _initialSyncRequestedFlag(false),
      _indexPrefetchConfig(PREFETCH_ALL) {}

BackgroundSync* BackgroundSync::get() {
    stdx::unique_lock<stdx::mutex> lock(s_mutex);
    if (s_instance == NULL && !inShutdown()) {
        s_instance = new BackgroundSync();
    }
    return s_instance;
}

void BackgroundSync::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // Clear the buffer in case the producerThread is waiting in push() due to a full queue.
    invariant(inShutdown());
    clearBuffer();
    _stopped = true;
}

void BackgroundSync::producerThread() {
    Client::initThread("rsBackgroundSync");
    AuthorizationSession::get(cc())->grantInternalAuthorization();

    _threadPoolTaskExecutor.startup();
    ON_BLOCK_EXIT([this]() {
        _threadPoolTaskExecutor.shutdown();
        _threadPoolTaskExecutor.join();
    });

    while (!inShutdown()) {
        try {
            _producerThread();
        } catch (const DBException& e) {
            std::string msg(str::stream() << "sync producer problem: " << e.toString());
            error() << msg;
            _replCoord->setMyHeartbeatMessage(msg);
            sleepmillis(100);  // sleep a bit to keep from hammering this thread with temp. errors.
        } catch (const std::exception& e2) {
            severe() << "sync producer exception: " << e2.what();
            fassertFailed(28546);
        }
    }
    stop();
}

void BackgroundSync::_signalNoNewDataForApplier() {
    // Signal to consumers that we have entered the stopped state
    // if the signal isn't already in the queue.
    const boost::optional<BSONObj> lastObjectPushed = _buffer.lastObjectPushed();
    if (!lastObjectPushed || !lastObjectPushed->isEmpty()) {
        const BSONObj sentinelDoc;
        _buffer.pushEvenIfFull(sentinelDoc);
        bufferCountGauge.increment();
        bufferSizeGauge.increment(sentinelDoc.objsize());
    }
}

void BackgroundSync::_producerThread() {
    const MemberState state = _replCoord->getMemberState();
    // Stop when the state changes to primary.
    if (_replCoord->isWaitingForApplierToDrain() || state.primary()) {
        if (!isStopped()) {
            stop();
        }
        if (_replCoord->isWaitingForApplierToDrain()) {
            _signalNoNewDataForApplier();
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

    invariant(!state.rollback());

    // We need to wait until initial sync has started.
    if (_replCoord->getMyLastAppliedOpTime().isNull()) {
        sleepsecs(1);
        return;
    }
    // we want to start when we're no longer primary
    // start() also loads _lastOpTimeFetched, which we know is set from the "if"
    OperationContextImpl txn;
    if (isStopped()) {
        start(&txn);
    }

    _produce(&txn);
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

        if (_replCoord->isWaitingForApplierToDrain() || _replCoord->getMemberState().primary() ||
            inShutdownStrict()) {
            return;
        }
    }

    HostAndPort oldSyncSource;
    // find a target to sync from the last optime fetched
    OpTime lastOpTimeFetched;
    {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        lastOpTimeFetched = _lastOpTimeFetched;
        oldSyncSource = _syncSourceHost;
        _syncSourceHost = HostAndPort();
    }
    OplogReader syncSourceReader;
    OpTime minValid;
    if (_replCoord->getMemberState().recovering()) {
        auto minValidSaved = getMinValid(txn);
        if (minValidSaved > lastOpTimeFetched) {
            minValid = minValidSaved;
        }
    }

    int rbid;
    syncSourceReader.connectToSyncSource(txn, lastOpTimeFetched, minValid, _replCoord, &rbid);

    // no server found
    if (syncSourceReader.getHost().empty()) {
        sleepsecs(1);
        // if there is no one to sync from
        return;
    }

    // If our sync source has not changed, it is likely caused by our heartbeat data map being
    // out of date. In that case we sleep for 1 second to reduce the amount we spin waiting
    // for our map to update.
    if (syncSourceReader.getHost() == oldSyncSource) {
        log() << "Chose same sync source candidate as last time, " << oldSyncSource
              << ". Sleeping for 1 second to avoid immediately choosing a new sync source for "
                 "the same reason as last time.";

        sleepsecs(1);
    }

    long long lastHashFetched;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_stopped) {
            return;
        }
        lastOpTimeFetched = _lastOpTimeFetched;
        lastHashFetched = _lastFetchedHash;
        _syncSourceHost = syncSourceReader.getHost();
        _replCoord->signalUpstreamUpdater();
    }

    const Milliseconds kRollbackOplogSocketTimeout(10 * 60 * 1000);

    const auto isV1ElectionProtocol = _replCoord->isV1ElectionProtocol();
    // Under protocol version 1, make the awaitData timeout (maxTimeMS) dependent on the election
    // timeout. This enables the sync source to communicate liveness of the primary to secondaries.
    // Under protocol version 0, use a default timeout of 2 seconds for awaitData.
    const Milliseconds fetcherMaxTimeMS(
        isV1ElectionProtocol ? _replCoord->getConfig().getElectionTimeoutPeriod() / 2 : Seconds(2));

    // Prefer host in oplog reader to _syncSourceHost because _syncSourceHost may be cleared
    // if sync source feedback fails.
    const HostAndPort source = syncSourceReader.getHost();
    syncSourceReader.resetConnection();
    // no more references to oplog reader from here on.

    // Set the applied point if unset. This is most likely the first time we've established a sync
    // source since stepping down or otherwise clearing the applied point. We need to set this here,
    // before the OplogWriter gets a chance to append to the oplog.
    if (getAppliedThrough(txn).isNull()) {
        setAppliedThrough(txn, _replCoord->getMyLastAppliedOpTime());
    }

    Status fetcherReturnStatus = Status::OK();
    auto fetcherCallback = stdx::bind(&BackgroundSync::_fetcherCallback,
                                      this,
                                      stdx::placeholders::_1,
                                      stdx::placeholders::_3,
                                      stdx::cref(source),
                                      lastOpTimeFetched,
                                      lastHashFetched,
                                      fetcherMaxTimeMS,
                                      &fetcherReturnStatus,
                                      rbid);


    BSONObjBuilder cmdBob;
    cmdBob.append("find", nsToCollectionSubstring(rsOplogName));
    cmdBob.append("filter", BSON("ts" << BSON("$gte" << lastOpTimeFetched.getTimestamp())));
    cmdBob.append("tailable", true);
    cmdBob.append("oplogReplay", true);
    cmdBob.append("awaitData", true);
    cmdBob.append("maxTimeMS", durationCount<Milliseconds>(Minutes(1)));  // 1 min initial find.

    BSONObjBuilder metadataBob;
    if (isV1ElectionProtocol) {
        cmdBob.append("term", _replCoord->getTerm());
        metadataBob.append(rpc::kReplSetMetadataFieldName, 1);
    }

    if (MONGO_FAIL_POINT(fetcherHangBeforeStart)) {
        // This log output is used in js tests so please leave it.
        log() << "BackgroundSync - fetcherHangBeforeStart fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(fetcherHangBeforeStart) && !inShutdown()) {
            mongo::sleepsecs(1);
        }

        // If the sync source candidate rolls back while in this fail point, it will close all
        // connections and the next request will fail.
        // We manually drop all connections here so that the following Fetcher request succeeds.
        _threadPoolTaskExecutor.dropConnections(source);
    }

    auto dbName = nsToDatabase(rsOplogName);
    auto cmdObj = cmdBob.obj();
    auto metadataObj = metadataBob.obj();
    // 5 seconds more than the find command's 1 minute maxTimeMs
    const Milliseconds oplogQueryNetworkTimeout = duration_cast<Milliseconds>(Seconds(65));
    Fetcher fetcher(&_threadPoolTaskExecutor,
                    source,
                    dbName,
                    cmdObj,
                    fetcherCallback,
                    metadataObj,
                    oplogQueryNetworkTimeout);

    LOG(1) << "scheduling fetcher to read remote oplog on " << source << " starting at "
           << cmdObj["filter"];
    auto scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        warning() << "unable to schedule fetcher to read remote oplog on " << source << ": "
                  << scheduleStatus;
        return;
    }
    fetcher.wait();
    LOG(1) << "fetcher stopped reading remote oplog on " << source;

    // If the background sync is stopped after the fetcher is started, we need to
    // re-evaluate our sync source and oplog common point.
    if (isStopped()) {
        return;
    }

    if (fetcherReturnStatus.code() == ErrorCodes::OplogOutOfOrder) {
        // This is bad because it means that our source
        // has not returned oplog entries in ascending ts order, and they need to be.

        warning() << fetcherReturnStatus.toString();
        // Do not blacklist the server here, it will be blacklisted when we try to reuse it,
        // if it can't return a matching oplog start from the last fetch oplog ts field.
        return;
    } else if (fetcherReturnStatus.code() == ErrorCodes::OplogStartMissing ||
               fetcherReturnStatus.code() == ErrorCodes::RemoteOplogStale) {
        // Rollback is a synchronous operation that uses the task executor and may not be
        // executed inside the fetcher callback.
        const int messagingPortTags = 0;
        ConnectionPool connectionPool(messagingPortTags);
        std::unique_ptr<ConnectionPool::ConnectionPtr> connection;
        auto getConnection =
            [&connection, &connectionPool, kRollbackOplogSocketTimeout, source]() -> DBClientBase* {
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

        log() << "Starting rollback due to " << fetcherReturnStatus;

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

        _rollback(txn, source, rbid, getConnection);
        stop();
    } else if (!fetcherReturnStatus.isOK()) {
        warning() << "Fetcher error querying oplog: " << fetcherReturnStatus.toString();
    }
}

void BackgroundSync::_lastAppliedFetcherCallback(const StatusWith<Fetcher::QueryResponse>& result,
                                                 OpTime lastOpTimeFetched,
                                                 Status* returnStatus) {
    if (!result.isOK()) {
        *returnStatus = result.getStatus();
        return;
    }

    const auto& queryResponse = result.getValue();
    if (queryResponse.documents.empty()) {
        *returnStatus = Status(ErrorCodes::InvalidSyncSource, "Upstream node had an empty oplog.");
        return;
    }

    const auto& remoteLastAppliedDocument = queryResponse.documents.front();
    const auto remoteLastAppliedOpTime = OpTime::parseFromOplogEntry(remoteLastAppliedDocument);
    if (!remoteLastAppliedOpTime.isOK()) {
        *returnStatus = Status(ErrorCodes::InvalidBSON,
                               str::stream() << "Received invalid oplog entry from upstream node: "
                                             << remoteLastAppliedDocument.toString() << ". Error: "
                                             << remoteLastAppliedOpTime.getStatus().toString());
        return;
    }
    if (remoteLastAppliedOpTime.getValue() <= lastOpTimeFetched) {
        *returnStatus = Status(ErrorCodes::InvalidSyncSource,
                               str::stream() << "Upstream node's last applied OpTime "
                                             << remoteLastAppliedOpTime.getValue().toString()
                                             << " is not greater than our last fetched OpTime "
                                             << lastOpTimeFetched.toString());
        return;
    }
}

void BackgroundSync::_fetcherCallback(const StatusWith<Fetcher::QueryResponse>& result,
                                      BSONObjBuilder* bob,
                                      const HostAndPort& source,
                                      OpTime lastOpTimeFetched,
                                      long long lastFetchedHash,
                                      Milliseconds fetcherMaxTimeMS,
                                      Status* returnStatus,
                                      int rbid) {
    // if target cut connections between connecting and querying (for
    // example, because it stepped down) we might not have a cursor
    if (!result.isOK()) {
        return;
    }

    if (inShutdown()) {
        return;
    }

    // Check if we have been stopped.
    if (isStopped()) {
        return;
    }

    const auto& queryResponse = result.getValue();
    bool syncSourceHasSyncSource = false;
    OpTime sourcesLastOp;

    const auto& documents = queryResponse.documents;
    auto firstDocToApply = documents.cbegin();
    auto lastDocToApply = documents.cend();

    if (!documents.empty()) {
        LOG(2) << "fetcher read " << documents.size()
               << " operations from remote oplog starting at " << documents.front()["ts"]
               << " and ending at " << documents.back()["ts"];
    } else {
        LOG(2) << "fetcher read 0 operations from remote oplog";
    }

    // Check start of remote oplog and, if necessary, stop fetcher to execute rollback.
    if (queryResponse.first) {
        // Once we establish our cursor, we need to ensure that our upstream node hasn't rolled back
        // since that could cause it to not have our required minValid point. The cursor will be
        // killed if the upstream node rolls back so we don't need to keep checking. This must be
        // blocking since the Fetcher doesn't give us a way to defer sending the getmores after we
        // return.
        auto handle = _threadPoolTaskExecutor.scheduleRemoteCommand(
            {source, "admin", BSON("replSetGetRBID" << 1)},
            [&](const executor::TaskExecutor::RemoteCommandCallbackArgs& rbidReply) {
                *returnStatus = rbidReply.response.getStatus();
                if (!returnStatus->isOK())
                    return;

                const auto& rbidReplyObj = rbidReply.response.getValue().data;
                *returnStatus = getStatusFromCommandResult(rbidReplyObj);
                if (!returnStatus->isOK())
                    return;

                const auto rbidElem = rbidReplyObj["rbid"];
                if (rbidElem.type() != NumberInt) {
                    *returnStatus =
                        Status(ErrorCodes::InvalidSyncSource,
                               str::stream() << "Upstream node returned an "
                                             << "rbid with invalid type " << rbidElem.type());
                    return;
                }
                if (rbidElem.Int() != rbid) {
                    *returnStatus = Status(ErrorCodes::InvalidSyncSource,
                                           "Upstream node rolled back after verifying "
                                           "that it had our MinValid point. Retrying.");
                }
            });
        if (!handle.isOK()) {
            *returnStatus = handle.getStatus();
            return;
        }

        _threadPoolTaskExecutor.wait(handle.getValue());
        if (!returnStatus->isOK())
            return;

        // Check that the upstream last applied OpTime is newer than our last fetched OpTime.
        Fetcher lastAppliedFetcher(&_threadPoolTaskExecutor,
                                   source,
                                   kLocalOplogNss.db().toString(),
                                   BSON("find" << kLocalOplogNss.coll() << "limit" << 1 << "sort"
                                               << BSON("$natural" << -1)),
                                   stdx::bind(&BackgroundSync::_lastAppliedFetcherCallback,
                                              this,
                                              stdx::placeholders::_1,
                                              lastOpTimeFetched,
                                              returnStatus),
                                   rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
                                   Seconds(30));
        lastAppliedFetcher.schedule();
        lastAppliedFetcher.wait();
        if (!returnStatus->isOK())
            return;

        auto getNextOperation = [&firstDocToApply, lastDocToApply]() -> StatusWith<BSONObj> {
            if (firstDocToApply == lastDocToApply) {
                return Status(ErrorCodes::OplogStartMissing, "remote oplog start missing");
            }
            return *(firstDocToApply++);
        };

        *returnStatus = checkRemoteOplogStart(getNextOperation, lastOpTimeFetched, lastFetchedHash);
        if (!returnStatus->isOK()) {
            // Stop fetcher and execute rollback.
            return;
        }

        // If this is the first batch and no rollback is needed, we should have advanced
        // the document iterator.
        invariant(firstDocToApply != documents.cbegin());
    }

    // No work to do if we are draining/primary.
    if (_replCoord->isWaitingForApplierToDrain() || _replCoord->getMemberState().primary()) {
        LOG(1) << "waiting for draining or we are primary, not adding more ops to buffer";
        return;
    }

    if (MONGO_FAIL_POINT(stopReplProducer)) {
        return;
    }

    // Process replset metadata.  It is important that this happen after we've validated the
    // first batch, so we don't progress our knowledge of the commit point from a
    // response that triggers a rollback.
    bool receivedMetadata =
        queryResponse.otherFields.metadata.hasElement(rpc::kReplSetMetadataFieldName);
    if (receivedMetadata) {
        auto metadataResult =
            rpc::ReplSetMetadata::readFromMetadata(queryResponse.otherFields.metadata);
        if (!metadataResult.isOK()) {
            error() << "invalid replication metadata from sync source " << source << ": "
                    << metadataResult.getStatus() << ": " << queryResponse.otherFields.metadata;
            return;
        }
        const auto& metadata = metadataResult.getValue();
        _replCoord->processReplSetMetadata(metadata, true /*advance commit point*/);
        if (metadata.getPrimaryIndex() != rpc::ReplSetMetadata::kNoPrimary) {
            _replCoord->cancelAndRescheduleElectionTimeout();
        }
        syncSourceHasSyncSource = metadata.getSyncSourceIndex() != -1;
        sourcesLastOp = metadata.getLastOpVisible();
    }

    // The count of the bytes of the documents read off the network.
    int networkDocumentBytes = 0;
    Timestamp lastTS;
    {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        // If we are stopped then return without queueing this batch to apply.
        if (_stopped) {
            return;
        }
        lastTS = _lastOpTimeFetched.getTimestamp();
    }
    int count = 0;
    for (auto&& doc : documents) {
        networkDocumentBytes += doc.objsize();
        ++count;

        // If this is the first response (to the $gte query) then we already applied the first doc.
        if (queryResponse.first && count == 1) {
            continue;
        }

        // Check to see if the oplog entry goes back in time for this document.
        const auto docOpTime = OpTime::parseFromOplogEntry(doc);
        fassertStatusOK(34362, docOpTime.getStatus());  // entries must have a "ts" field.
        const auto docTS = docOpTime.getValue().getTimestamp();

        if (lastTS >= docTS) {
            *returnStatus = Status(
                ErrorCodes::OplogOutOfOrder,
                str::stream() << "Reading the oplog from" << source.toString()
                              << " returned out of order entries. lastTS: " << lastTS.toString()
                              << " outOfOrderTS:" << docTS.toString() << " at count:" << count);
            return;
        }
        lastTS = docTS;
    }

    // These numbers are for the documents we will apply.
    auto toApplyDocumentCount = documents.size();
    auto toApplyDocumentBytes = networkDocumentBytes;
    if (queryResponse.first) {
        // The count is one less since the first document found was already applied ($gte $ts query)
        // and we will not apply it again. We just needed to check it so we didn't rollback, or
        // error above.
        --toApplyDocumentCount;
        const auto alreadyAppliedDocument = documents.cbegin();
        toApplyDocumentBytes -= alreadyAppliedDocument->objsize();
    }

    if (toApplyDocumentBytes > 0) {
        // Wait for enough space.
        _buffer.waitForSpace(toApplyDocumentBytes);

        OCCASIONALLY {
            LOG(2) << "bgsync buffer has " << _buffer.size() << " bytes";
        }

        // Buffer docs for later application.
        std::vector<BSONObj> objs{firstDocToApply, lastDocToApply};
        _buffer.pushAllNonBlocking(objs);

        // Inc stats.
        opsReadStats.increment(documents.size());  // we read all of the docs in the query.
        networkByteStats.increment(networkDocumentBytes);
        bufferCountGauge.increment(toApplyDocumentCount);
        bufferSizeGauge.increment(toApplyDocumentBytes);

        // Update last fetched info.
        auto lastDoc = objs.back();
        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            _lastFetchedHash = lastDoc["h"].numberLong();
            _lastOpTimeFetched = fassertStatusOK(28770, OpTime::parseFromOplogEntry(lastDoc));
            LOG(3) << "batch resetting _lastOpTimeFetched: " << _lastOpTimeFetched;
        }
    }

    // record time for each batch
    getmoreReplStats.recordMillis(durationCount<Milliseconds>(queryResponse.elapsedMillis));

    // Check some things periodically
    // (whenever we run out of items in the
    // current cursor batch)
    if (networkDocumentBytes > 0 && networkDocumentBytes < BatchIsSmallish) {
        // on a very low latency network, if we don't wait a little, we'll be
        // getting ops to write almost one at a time.  this will both be expensive
        // for the upstream server as well as potentially defeating our parallel
        // application of batches on the secondary.
        //
        // the inference here is basically if the batch is really small, we are
        // "caught up".
        //
        sleepmillis(SleepToAllowBatchingMillis);
    }

    if (inShutdown()) {
        return;
    }

    // If we are transitioning to primary state, we need to leave
    // this loop in order to go into bgsync-stop mode.
    if (_replCoord->isWaitingForApplierToDrain() || _replCoord->getMemberState().primary()) {
        return;
    }

    // re-evaluate quality of sync target
    if (_shouldChangeSyncSource(source, sourcesLastOp, syncSourceHasSyncSource)) {
        return;
    }

    // Check if we have been stopped.
    if (isStopped()) {
        return;
    }

    // We fill in 'bob' to signal the fetcher to process with another getMore, if needed.
    if (bob) {
        bob->append("getMore", queryResponse.cursorId);
        bob->append("collection", queryResponse.nss.coll());
        bob->append("maxTimeMS", durationCount<Milliseconds>(fetcherMaxTimeMS));
        if (receivedMetadata) {
            bob->append("term", _replCoord->getTerm());
            _replCoord->getLastCommittedOpTime().append(bob, "lastKnownCommittedOpTime");
        }
    }
}

bool BackgroundSync::_shouldChangeSyncSource(const HostAndPort& syncSource,
                                             const OpTime& syncSourceLastOpTime,
                                             bool syncSourceHasSyncSource) {
    // is it even still around?
    if (getSyncTarget().empty() || syncSource.empty()) {
        return true;
    }

    // check other members: is any member's optime more than MaxSyncSourceLag seconds
    // ahead of the current sync source?
    return _replCoord->shouldChangeSyncSource(
        syncSource, syncSourceLastOpTime, syncSourceHasSyncSource);
}


bool BackgroundSync::peek(BSONObj* op) {
    return _buffer.peek(*op);
}

void BackgroundSync::waitForMore() {
    BSONObj op;
    // Block for one second before timing out.
    // Ignore the value of the op we peeked at.
    _buffer.blockingPeek(op, 1);
}

void BackgroundSync::consume() {
    // this is just to get the op off the queue, it's been peeked at
    // and queued for application already
    BSONObj op = _buffer.blockingPop();
    bufferCountGauge.decrement(1);
    bufferSizeGauge.decrement(getSize(op));
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
            severe() << "Unable to complete rollback. A full resync may be needed: " << status;
            fassertFailedNoTrace(28723);
        }

        // In other cases, we log the message contained in the error status and retry later.
        uassertStatusOK(status);
    } catch (const DBException& ex) {
        // UnrecoverableRollbackError should only come from a returned status which is handled
        // above.
        invariant(ex.getCode() != ErrorCodes::UnrecoverableRollbackError);

        warning() << "rollback cannot complete at this time (retrying later): " << ex
                  << " appliedThrough=" << _replCoord->getMyLastAppliedOpTime()
                  << " minvalid=" << getMinValid(txn);

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

    if (!_replCoord->setFollowerMode(MemberState::RS_RECOVERING)) {
        severe() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                 << "; expected to be in state " << MemberState(MemberState::RS_ROLLBACK)
                 << " but found self in " << _replCoord->getMemberState();
        fassertFailedNoTrace(40364);
    }
}

HostAndPort BackgroundSync::getSyncTarget() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _syncSourceHost;
}

void BackgroundSync::clearSyncTarget() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _syncSourceHost = HostAndPort();
}

void BackgroundSync::cancelFetcher() {
    _threadPoolTaskExecutor.cancelAllCommands();
}

void BackgroundSync::stop() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _stopped = true;
    _syncSourceHost = HostAndPort();
    _lastOpTimeFetched = OpTime();
    _lastFetchedHash = 0;
}

void BackgroundSync::start(OperationContext* txn) {
    massert(16235, "going to start syncing, but buffer is not empty", _buffer.empty());

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

void BackgroundSync::clearBuffer() {
    _buffer.clear();
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
        severe() << "Problem reading " << rsOplogName << ": " << ex.toStatus();
        fassertFailed(18904);
    }
    BSONElement hashElement = oplogEntry[hashFieldName];
    if (hashElement.eoo()) {
        severe() << "Most recent entry in " << rsOplogName << " missing \"" << hashFieldName
                 << "\" field";
        fassertFailed(18902);
    }
    if (hashElement.type() != NumberLong) {
        severe() << "Expected type of \"" << hashFieldName << "\" in most recent " << rsOplogName
                 << " entry to have type NumberLong, but found " << typeName(hashElement.type());
        fassertFailed(18903);
    }
    return hashElement.safeNumberLong();
}

bool BackgroundSync::getInitialSyncRequestedFlag() {
    stdx::lock_guard<stdx::mutex> lock(_initialSyncMutex);
    return _initialSyncRequestedFlag;
}

void BackgroundSync::setInitialSyncRequestedFlag(bool value) {
    stdx::lock_guard<stdx::mutex> lock(_initialSyncMutex);
    _initialSyncRequestedFlag = value;
}

void BackgroundSync::pushTestOpToBuffer(const BSONObj& op) {
    _buffer.push(op);
    bufferCountGauge.increment();
    bufferSizeGauge.increment(op.objsize());
}


}  // namespace repl
}  // namespace mongo
