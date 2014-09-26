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
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace repl {

namespace {
    const char hashFieldName[] = "h";
    int SleepToAllowBatchingMillis = 2;
    const int BatchIsSmallish = 40000; // bytes
} // namespace

    MONGO_FP_DECLARE(rsBgSyncProduce);

    BackgroundSync* BackgroundSync::s_instance = 0;
    boost::mutex BackgroundSync::s_mutex;

    //The number and time spent reading batches off the network
    static TimerStats getmoreReplStats;
    static ServerStatusMetricField<TimerStats> displayBatchesRecieved(
                                                    "repl.network.getmores",
                                                    &getmoreReplStats );
    //The oplog entries read via the oplog reader
    static Counter64 opsReadStats;
    static ServerStatusMetricField<Counter64> displayOpsRead( "repl.network.ops",
                                                                &opsReadStats );
    //The bytes read via the oplog reader
    static Counter64 networkByteStats;
    static ServerStatusMetricField<Counter64> displayBytesRead( "repl.network.bytes",
                                                                &networkByteStats );

    //The count of items in the buffer
    static Counter64 bufferCountGauge;
    static ServerStatusMetricField<Counter64> displayBufferCount( "repl.buffer.count",
                                                                &bufferCountGauge );
    //The size (bytes) of items in the buffer
    static Counter64 bufferSizeGauge;
    static ServerStatusMetricField<Counter64> displayBufferSize( "repl.buffer.sizeBytes",
                                                                &bufferSizeGauge );
    //The max size (bytes) of the buffer
    static int bufferMaxSizeGauge = 256*1024*1024;
    static ServerStatusMetricField<int> displayBufferMaxSize( "repl.buffer.maxSizeBytes",
                                                                &bufferMaxSizeGauge );


    BackgroundSyncInterface::~BackgroundSyncInterface() {}

    size_t getSize(const BSONObj& o) {
        // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
        return static_cast<size_t>(o.objsize());
    }

    BackgroundSync::BackgroundSync() : _buffer(bufferMaxSizeGauge, &getSize),
                                       _lastOpTimeFetched(std::numeric_limits<int>::max(),
                                                          0),
                                       _lastAppliedHash(0),
                                       _lastFetchedHash(0),
                                       _pause(true),
                                       _appliedBuffer(true),
                                       _assumingPrimary(false),
                                       _replCoord(getGlobalReplicationCoordinator()),
                                       _initialSyncRequestedFlag(false) {
    }

    BackgroundSync* BackgroundSync::get() {
        boost::unique_lock<boost::mutex> lock(s_mutex);
        if (s_instance == NULL && !inShutdown()) {
            s_instance = new BackgroundSync();
        }
        return s_instance;
    }

    void BackgroundSync::shutdown() {
        notify();
    }

    void BackgroundSync::notify() {
        boost::lock_guard<boost::mutex> lock(_mutex);

        // If all ops in the buffer have been applied, unblock waitForRepl (if it's waiting)
        if (_buffer.empty()) {
            _appliedBuffer = true;
            _condvar.notify_all();
        }
    }

    void BackgroundSync::producerThread() {
        Client::initThread("rsBackgroundSync");
        replLocalAuth();

        while (!inShutdown()) {
            try {
                _producerThread();
            }
            catch (const DBException& e) {
                sethbmsg(str::stream() << "sync source problem: " << e.toString());
            }
            catch (const std::exception& e2) {
                sethbmsg(str::stream() << "exception in producer: " << e2.what());
                sleepsecs(60);
            }
        }

        cc().shutdown();
    }

    void BackgroundSync::_producerThread() {
        MemberState state = theReplSet->state();

        // we want to pause when the state changes to primary
        if (isAssumingPrimary() || state.primary()) {
            if (!_pause) {
                stop();
            }
            sleepsecs(1);
            return;
        }

        if (state.startup()) {
            sleepsecs(1);
            return;
        }

        OperationContextImpl txn;

        // We need to wait until initial sync has started.
        if (_replCoord->getMyLastOptime().isNull()) {
            sleepsecs(1);
            return;
        }
        // we want to unpause when we're no longer primary
        // start() also loads _lastOpTimeFetched, which we know is set from the "if"
        else if (_pause) {
            start(&txn);
        }

        produce(&txn);
    }

    void BackgroundSync::produce(OperationContext* txn) {
        // this oplog reader does not do a handshake because we don't want the server it's syncing
        // from to track how far it has synced
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            if (_lastOpTimeFetched.isNull()) {
                // then we're initial syncing and we're still waiting for this to be set
                lock.unlock();
                sleepsecs(1);
                // if there is no one to sync from
                return;
            }

            // Wait until we've applied the ops we have before we choose a sync target
            while (!_appliedBuffer) {
                _condvar.wait(lock);
            }
        }

        while (MONGO_FAIL_POINT(rsBgSyncProduce)) {
            sleepmillis(0);
        }


        // find a target to sync from the last optime fetched
        OpTime lastOpTimeFetched;
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            lastOpTimeFetched = _lastOpTimeFetched;
            _syncSourceHost = HostAndPort();
        }
        _syncSourceReader.resetConnection();
        _syncSourceReader.connectToSyncSource(txn, lastOpTimeFetched, _replCoord);

        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            // no server found
            if (_syncSourceReader.getHost().empty()) {
                lock.unlock();
                sleepsecs(1);
                // if there is no one to sync from
                return;
            }
            lastOpTimeFetched = _lastOpTimeFetched;
            _syncSourceHost = _syncSourceReader.getHost();
        }

        _syncSourceReader.tailingQueryGTE(rsoplog, lastOpTimeFetched);

        // if target cut connections between connecting and querying (for
        // example, because it stepped down) we might not have a cursor
        if (!_syncSourceReader.haveCursor()) {
            return;
        }

        if (_rollbackIfNeeded(txn, _syncSourceReader)) {
            stop();
            return;
        }

        while (!inShutdown()) {
            if (!_syncSourceReader.moreInCurrentBatch()) {
                // Check some things periodically
                // (whenever we run out of items in the
                // current cursor batch)

                int bs = _syncSourceReader.currentBatchMessageSize();
                if( bs > 0 && bs < BatchIsSmallish ) {
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
  
                if (theReplSet->gotForceSync()) {
                    return;
                }
                // If we are transitioning to primary state, we need to leave
                // this loop in order to go into bgsync-pause mode.
                if (isAssumingPrimary() || theReplSet->isPrimary()) {
                    return;
                }

                // re-evaluate quality of sync target
                if (shouldChangeSyncSource()) {
                    return;
                }

                {
                    //record time for each getmore
                    TimerHolder batchTimer(&getmoreReplStats);
                    
                    // This calls receiveMore() on the oplogreader cursor.
                    // It can wait up to five seconds for more data.
                    _syncSourceReader.more();
                }
                networkByteStats.increment(_syncSourceReader.currentBatchMessageSize());

                if (!_syncSourceReader.moreInCurrentBatch()) {
                    // If there is still no data from upstream, check a few more things
                    // and then loop back for another pass at getting more data
                    {
                        boost::unique_lock<boost::mutex> lock(_mutex);
                        if (_pause) {
                            return;
                        }
                    }

                    _syncSourceReader.tailCheck();
                    if( !_syncSourceReader.haveCursor() ) {
                        LOG(1) << "replSet end syncTail pass" << rsLog;
                        return;
                    }

                    continue;
                }
            }

            // At this point, we are guaranteed to have at least one thing to read out
            // of the oplogreader cursor.
            BSONObj o = _syncSourceReader.nextSafe().getOwned();
            opsReadStats.increment();

            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _appliedBuffer = false;
            }

            OCCASIONALLY {
                LOG(2) << "bgsync buffer has " << _buffer.size() << " bytes" << rsLog;
            }
            // the blocking queue will wait (forever) until there's room for us to push
            _buffer.push(o);
            bufferCountGauge.increment();
            bufferSizeGauge.increment(getSize(o));

            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _lastFetchedHash = o["h"].numberLong();
                _lastOpTimeFetched = o["ts"]._opTime();
                LOG(3) << "replSet lastOpTimeFetched: "
                       << _lastOpTimeFetched.toStringPretty() << rsLog;
            }
        }
    }

    bool BackgroundSync::shouldChangeSyncSource() {
        // is it even still around?
        if (_syncSourceReader.getHost().empty()) {
            return true;
        }

        // check other members: is any member's optime more than MaxSyncSourceLag seconds 
        // ahead of the current sync source?
        return _replCoord->shouldChangeSyncSource(_syncSourceReader.getHost());
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

    bool BackgroundSync::isStale(OpTime lastOpTimeFetched, 
                                 OplogReader& r, 
                                 BSONObj& remoteOldestOp) {
        remoteOldestOp = r.findOne(rsoplog, Query());
        OpTime remoteTs = remoteOldestOp["ts"]._opTime();
        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (lastOpTimeFetched >= remoteTs) {
                return false;
            }
            log() << "replSet remoteOldestOp:    " << remoteTs.toStringLong() << rsLog;
            log() << "replSet lastOpTimeFetched: " << lastOpTimeFetched.toStringLong() << rsLog;
        }

        return true;
    }

    bool BackgroundSync::_rollbackIfNeeded(OperationContext* txn, OplogReader& r) {
        string hn = r.conn()->getServerAddress();

        if (!r.more()) {
            try {
                BSONObj theirLastOp = r.getLastOp(rsoplog);
                if (theirLastOp.isEmpty()) {
                    log() << "replSet error empty query result from " << hn << " oplog" << rsLog;
                    sleepsecs(2);
                    return true;
                }
                OpTime theirTS = theirLastOp["ts"]._opTime();
                if (theirTS < _lastOpTimeFetched) {
                    log() << "replSet we are ahead of the sync source, will try to roll back"
                          << rsLog;
                    syncRollback(txn, _replCoord->getMyLastOptime(), &r, _replCoord);
                    return true;
                }
                /* we're not ahead?  maybe our new query got fresher data.  best to come back and try again */
                log() << "replSet syncTail condition 1" << rsLog;
                sleepsecs(1);
            }
            catch(DBException& e) {
                log() << "replSet error querying " << hn << ' ' << e.toString() << rsLog;
                sleepsecs(2);
            }
            return true;
        }

        BSONObj o = r.nextSafe();
        OpTime ts = o["ts"]._opTime();
        long long hash = o["h"].numberLong();
        if( ts != _lastOpTimeFetched || hash != _lastFetchedHash ) {
            log() << "replSet our last op time fetched: " << _lastOpTimeFetched.toStringPretty() << rsLog;
            log() << "replset source's GTE: " << ts.toStringPretty() << rsLog;
            syncRollback(txn, _replCoord->getMyLastOptime(), &r, _replCoord);
            return true;
        }

        return false;
    }

    HostAndPort BackgroundSync::getSyncTarget() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _syncSourceHost;
    }

    void BackgroundSync::clearSyncTarget() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        _syncSourceReader.resetConnection();
        _syncSourceHost = HostAndPort();
    }

    void BackgroundSync::stop() {
        boost::unique_lock<boost::mutex> lock(_mutex);

        _pause = true;
        _syncSourceReader.resetConnection();
        _syncSourceHost = HostAndPort();
        _lastOpTimeFetched = OpTime(0,0);
        _lastFetchedHash = 0;
        _condvar.notify_all();
    }

    void BackgroundSync::start(OperationContext* txn) {
        massert(16235, "going to start syncing, but buffer is not empty", _buffer.empty());

        boost::unique_lock<boost::mutex> lock(_mutex);
        _pause = false;

        // reset _last fields with current oplog data
        _lastOpTimeFetched = _replCoord->getMyLastOptime();
        loadLastAppliedHash(txn);
        _lastFetchedHash = _lastAppliedHash;

        LOG(1) << "replset bgsync fetch queue set to: " << _lastOpTimeFetched << 
            " " << _lastFetchedHash << rsLog;
    }

    bool BackgroundSync::isAssumingPrimary() {
        boost::unique_lock<boost::mutex> lck(_mutex);
        return _assumingPrimary;
    }

    void BackgroundSync::stopReplicationAndFlushBuffer() {
        boost::unique_lock<boost::mutex> lck(_mutex);

        // 1. Tell syncing to stop
        _assumingPrimary = true;

        // 2. Wait for syncing to stop and buffer to be applied
        while (!(_pause && _appliedBuffer)) {
            _condvar.wait(lck);
        }

        // 3. Now actually become primary
        _assumingPrimary = false;
    }

    long long BackgroundSync::getLastAppliedHash() const {
        boost::lock_guard<boost::mutex> lck(_mutex);
        return _lastAppliedHash;
    }

    void BackgroundSync::setLastAppliedHash(long long newHash) {
        boost::lock_guard<boost::mutex> lck(_mutex);
        _lastAppliedHash = newHash;
    }

    void BackgroundSync::loadLastAppliedHash(OperationContext* txn) {
        Lock::DBRead lk(txn->lockState(), rsoplog);
        BSONObj oplogEntry;
        try {
            if (!Helpers::getLast(txn, rsoplog, oplogEntry)) {
                // This can happen when we are to do an initial sync.  lastHash will be set
                // after the initial sync is complete.
                _lastAppliedHash = 0;
                return;
            }
        }
        catch (const DBException& ex) {
            severe() << "Problem reading " << rsoplog << ": " << ex.toStatus();
            fassertFailed(18904);
        }
        BSONElement hashElement = oplogEntry[hashFieldName];
        if (hashElement.eoo()) {
            severe() << "Most recent entry in " << rsoplog << " missing \"" << hashFieldName <<
                "\" field";
            fassertFailed(18902);
        }
        if (hashElement.type() != NumberLong) {
            severe() << "Expected type of \"" << hashFieldName << "\" in most recent " << 
                rsoplog << " entry to have type NumberLong, but found " << 
                typeName(hashElement.type());
            fassertFailed(18903);
        }
        _lastAppliedHash = hashElement.safeNumberLong();
    }

    bool BackgroundSync::getInitialSyncRequestedFlag() {
        boost::lock_guard<boost::mutex> lock(_initialSyncMutex);
        return _initialSyncRequestedFlag;
    }

    void BackgroundSync::setInitialSyncRequestedFlag(bool value) {
        boost::lock_guard<boost::mutex> lock(_initialSyncMutex);
        _initialSyncRequestedFlag = value;
    }


} // namespace repl
} // namespace mongo
