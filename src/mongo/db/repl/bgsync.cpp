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

#include "mongo/pch.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/base/counter.h"
#include "mongo/db/stats/timer_stats.h"

namespace mongo {
namespace replset {

    int SleepToAllowBatchingMillis = 2;
    const int BatchIsSmallish = 40000; // bytes

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
                                       _lastOpTimeFetched(0, 0),
                                       _lastH(0),
                                       _pause(true),
                                       _appliedBuffer(true),
                                       _assumingPrimary(false),
                                       _currentSyncTarget(NULL),
                                       _oplogMarkerTarget(NULL),
                                       _consumedOpTime(0, 0) {
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
        {
            boost::unique_lock<boost::mutex> lock(s_mutex);
            if (s_instance == NULL) {
                return;
            }
        }

        {
            boost::unique_lock<boost::mutex> opLock(s_instance->_lastOpMutex);
            s_instance->_lastOpCond.notify_all();
        }

        {
            boost::unique_lock<boost::mutex> lock(s_instance->_mutex);

            // If all ops in the buffer have been applied, unblock waitForRepl (if it's waiting)
            if (s_instance->_buffer.empty()) {
                s_instance->_appliedBuffer = true;
                s_instance->_condvar.notify_all();
            }
        }
    }

    void BackgroundSync::notifierThread() {
        Client::initThread("rsSyncNotifier");
        theReplSet->syncSourceFeedback.ensureMe();
        replLocalAuth();

        // This makes the initial connection to our sync source for oplog position notification.
        // It also sets the supportsUpdater flag so we know which method to use.
        // If this function fails, we ignore that situation because it will be taken care of
        // the first time markOplog() is called in the loop below.
        connectOplogNotifier();
        theReplSet->syncSourceFeedback.go();

        while (!inShutdown()) {
            bool clearTarget = false;

            if (!theReplSet) {
                sleepsecs(5);
                continue;
            }

            MemberState state = theReplSet->state();
            if (state.primary() || state.fatal() || state.startup()) {
                sleepsecs(5);
                continue;
            }

            try {
                {
                    boost::unique_lock<boost::mutex> lock(_lastOpMutex);
                    while (_consumedOpTime == theReplSet->lastOpTimeWritten) {
                        _lastOpCond.wait(lock);
                    }
                }

                markOplog();
            }
            catch (DBException &e) {
                clearTarget = true;
                log() << "replset tracking exception: " << e.getInfo() << rsLog;
                sleepsecs(1);
            }
            catch (std::exception &e2) {
                clearTarget = true;
                log() << "replset tracking error" << e2.what() << rsLog;
                sleepsecs(1);
            }

            if (clearTarget) {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _oplogMarkerTarget = NULL;
            }
        }

        cc().shutdown();
    }

    void BackgroundSync::markOplog() {
        LOG(3) << "replset markOplog: " << _consumedOpTime << " "
               << theReplSet->lastOpTimeWritten << rsLog;

        if (theReplSet->syncSourceFeedback.supportsUpdater()) {
            _consumedOpTime = theReplSet->lastOpTimeWritten;
            theReplSet->syncSourceFeedback.updateSelfInMap(theReplSet->lastOpTimeWritten);
        }
        else {
            if (!hasCursor()) {
                sleepmillis(500);
                return;
            }

            if (!theReplSet->syncSourceFeedback.moreInCurrentBatch()) {
                theReplSet->syncSourceFeedback.more();
            }

            if (!theReplSet->syncSourceFeedback.more()) {
                theReplSet->syncSourceFeedback.tailCheck();
                return;
            }

            // if this member has written the op at optime T
            // we want to nextSafe up to and including T
            while (_consumedOpTime < theReplSet->lastOpTimeWritten
                   && theReplSet->syncSourceFeedback.more()) {
                BSONObj temp = theReplSet->syncSourceFeedback.nextSafe();
                _consumedOpTime = temp["ts"]._opTime();
            }

            // call more() to signal the sync target that we've synced T
            theReplSet->syncSourceFeedback.more();
        }
    }

    bool BackgroundSync::connectOplogNotifier() {
        boost::unique_lock<boost::mutex> lock(_mutex);

        if (!_oplogMarkerTarget || _currentSyncTarget != _oplogMarkerTarget) {
            if (!_currentSyncTarget) {
                return false;
            }

            log() << "replset setting oplog notifier to "
                  << _currentSyncTarget->fullName() << rsLog;
            _oplogMarkerTarget = _currentSyncTarget;

            if (!theReplSet->syncSourceFeedback.connect(_oplogMarkerTarget)) {
                _oplogMarkerTarget = NULL;
                return false;
            }
        }
        return true;
    }

    bool BackgroundSync::hasCursor() {
        if (!connectOplogNotifier()) {
            return false;
        }
        if (!theReplSet->syncSourceFeedback.haveCursor()) {
            BSONObj fields = BSON("ts" << 1);
            theReplSet->syncSourceFeedback.tailingQueryGTE(rsoplog,
                                                theReplSet->lastOpTimeWritten, &fields);
        }

        return theReplSet->syncSourceFeedback.haveCursor();
    }

    void BackgroundSync::producerThread() {
        Client::initThread("rsBackgroundSync");
        replLocalAuth();

        while (!inShutdown()) {
            if (!theReplSet) {
                log() << "replSet warning did not receive a valid config yet, sleeping 20 seconds " << rsLog;
                sleepsecs(20);
                continue;
            }

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

        if (state.fatal() || state.startup()) {
            sleepsecs(5);
            return;
        }

        // if this member has an empty oplog, we cannot start syncing
        if (theReplSet->lastOpTimeWritten.isNull()) {
            sleepsecs(1);
            return;
        }
        // we want to unpause when we're no longer primary
        // start() also loads _lastOpTimeFetched, which we know is set from the "if"
        else if (_pause) {
            start();
        }

        produce();
    }

    void BackgroundSync::produce() {
        // this oplog reader does not do a handshake because we don't want the server it's syncing
        // from to track how far it has synced
        OplogReader r;
        OpTime lastOpTimeFetched;
        // find a target to sync from the last op time written
        getOplogReader(r);

        // no server found
        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (_currentSyncTarget == NULL) {
                lock.unlock();
                sleepsecs(1);
                // if there is no one to sync from
                return;
            }
            lastOpTimeFetched = _lastOpTimeFetched;
        }

        r.tailingQueryGTE(rsoplog, lastOpTimeFetched);

        // if target cut connections between connecting and querying (for
        // example, because it stepped down) we might not have a cursor
        if (!r.haveCursor()) {
            return;
        }

        uassert(1000, "replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?", r.awaitCapable() );

        if (isRollbackRequired(r)) {
            stop();
            return;
        }

        while (!inShutdown()) {
            if (!r.moreInCurrentBatch()) {
                // Check some things periodically
                // (whenever we run out of items in the
                // current cursor batch)

                int bs = r.currentBatchMessageSize();
                if( bs > 0 && bs < BatchIsSmallish ) {
                    // on a very low latency network, if we don't wait a little, we'll be 
                    // getting ops to write almost one at a time.  this will both be expensive
                    // for the upstream server as well as potentially defeating our parallel 
                    // application of batches on the secondary.
                    //
                    // the inference here is basically if the batch is really small, we are 
                    // "caught up".
                    //
                    dassert( !Lock::isLocked() );
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
                if (shouldChangeSyncTarget()) {
                    return;
                }


                {
                    //record time for each getmore
                    TimerHolder batchTimer(&getmoreReplStats);
                    
                    // This calls receiveMore() on the oplogreader cursor.
                    // It can wait up to five seconds for more data.
                    r.more();
                }
                networkByteStats.increment(r.currentBatchMessageSize());

                if (!r.moreInCurrentBatch()) {
                    // If there is still no data from upstream, check a few more things
                    // and then loop back for another pass at getting more data
                    {
                        boost::unique_lock<boost::mutex> lock(_mutex);
                        if (_pause || 
                            !_currentSyncTarget || 
                            !_currentSyncTarget->hbinfo().hbstate.readable()) {
                            return;
                        }
                    }

                    r.tailCheck();
                    if( !r.haveCursor() ) {
                        LOG(1) << "replSet end syncTail pass" << rsLog;
                        return;
                    }

                    continue;
                }
            }

            // At this point, we are guaranteed to have at least one thing to read out
            // of the oplogreader cursor.
            BSONObj o = r.nextSafe().getOwned();
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
                _lastH = o["h"].numberLong();
                _lastOpTimeFetched = o["ts"]._opTime();
            }
        }
    }

    bool BackgroundSync::shouldChangeSyncTarget() {
        boost::unique_lock<boost::mutex> lock(_mutex);

        // is it even still around?
        if (!_currentSyncTarget || !_currentSyncTarget->hbinfo().hbstate.readable()) {
            return true;
        }

        // check other members: is any member's optime more than 30 seconds ahead of the guy we're
        // syncing from?
        return theReplSet->shouldChangeSyncTarget(_currentSyncTarget->hbinfo().opTime);
    }


    bool BackgroundSync::peek(BSONObj* op) {
        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (_currentSyncTarget != _oplogMarkerTarget &&
                _currentSyncTarget != NULL) {
                _oplogMarkerTarget = NULL;
            }
        }
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

    bool BackgroundSync::isStale(OplogReader& r, BSONObj& remoteOldestOp) {
        remoteOldestOp = r.findOne(rsoplog, Query());
        OpTime remoteTs = remoteOldestOp["ts"]._opTime();
        DEV {
            log() << "replSet remoteOldestOp:    " << remoteTs.toStringLong() << rsLog;
            log() << "replSet lastOpTimeFetched: " << _lastOpTimeFetched.toStringLong() << rsLog;
        }
        LOG(3) << "replSet remoteOldestOp: " << remoteTs.toStringLong() << rsLog;

        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (_lastOpTimeFetched >= remoteTs) {
                return false;
            }
        }

        return true;
    }

    void BackgroundSync::getOplogReader(OplogReader& r) {
        const Member *target = NULL, *stale = NULL;
        BSONObj oldest;

        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            if (_lastOpTimeFetched.isNull()) {
                // then we're initial syncing and we're still waiting for this to be set
                _currentSyncTarget = NULL;
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

        verify(r.conn() == NULL);

        while ((target = theReplSet->getMemberToSyncTo()) != NULL) {
            string current = target->fullName();

            if (!r.connect(current)) {
                LOG(2) << "replSet can't connect to " << current << " to read operations" << rsLog;
                r.resetConnection();
                theReplSet->veto(current);
                sleepsecs(1);
                continue;
            }

            if (isStale(r, oldest)) {
                r.resetConnection();
                theReplSet->veto(current, 600);
                stale = target;
                continue;
            }

            // if we made it here, the target is up and not stale
            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _currentSyncTarget = target;
            }
            theReplSet->syncSourceFeedback.connect(target);

            return;
        }

        // the only viable sync target was stale
        if (stale) {
            theReplSet->goStale(stale, oldest);
            sleepsecs(120);
        }

        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            _currentSyncTarget = NULL;
        }
    }

    bool BackgroundSync::isRollbackRequired(OplogReader& r) {
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
                    theReplSet->syncRollback(r);
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
        long long h = o["h"].numberLong();
        if( ts != _lastOpTimeFetched || h != _lastH ) {
            log() << "replSet our last op time fetched: " << _lastOpTimeFetched.toStringPretty() << rsLog;
            log() << "replset source's GTE: " << ts.toStringPretty() << rsLog;
            theReplSet->syncRollback(r);
            return true;
        }

        return false;
    }

    const Member* BackgroundSync::getSyncTarget() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _currentSyncTarget;
    }

    void BackgroundSync::stop() {
        boost::unique_lock<boost::mutex> lock(_mutex);

        _pause = true;
        _currentSyncTarget = NULL;
        _lastOpTimeFetched = OpTime(0,0);
        _lastH = 0;
        _condvar.notify_all();
    }

    void BackgroundSync::start() {
        massert(16235, "going to start syncing, but buffer is not empty", _buffer.empty());

        boost::unique_lock<boost::mutex> lock(_mutex);
        _pause = false;

        // reset _last fields with current data
        _lastOpTimeFetched = theReplSet->lastOpTimeWritten;
        _lastH = theReplSet->lastH;

        LOG(1) << "replset bgsync fetch queue set to: " << _lastOpTimeFetched << " " << _lastH << rsLog;
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

} // namespace replset
} // namespace mongo
