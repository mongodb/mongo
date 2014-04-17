/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/repl/rs_sync.h"

#include <vector>

#include "third_party/murmurhash3/MurmurHash3.h"

#include "mongo/base/counter.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

    using namespace bson;

    // For testing network failures in percolate() for chaining
    MONGO_FP_DECLARE(rsChaining1);
    MONGO_FP_DECLARE(rsChaining2);
    MONGO_FP_DECLARE(rsChaining3);

    MONGO_EXPORT_STARTUP_SERVER_PARAMETER(maxSyncSourceLagSecs, int, 30);
    MONGO_INITIALIZER(maxSyncSourceLagSecsCheck) (InitializerContext*) {
        if (maxSyncSourceLagSecs < 1) {
            return Status(ErrorCodes::BadValue, "maxSyncSourceLagSecs must be > 0");
        }
        return Status::OK();
    }

    /* should be in RECOVERING state on arrival here.
       readlocks
       @return true if transitioned to SECONDARY
    */
    bool ReplSetImpl::tryToGoLiveAsASecondary(OpTime& /*out*/ minvalid) {
        bool golive = false;

        lock rsLock( this );

        if (_maintenanceMode > 0) {
            // we're not actually going live
            return true;
        }

        // if we're blocking sync, don't change state
        if (_blockSync) {
            return false;
        }

        Lock::GlobalWrite writeLock;

        // make sure we're not primary, secondary, rollback, or fatal already
        if (box.getState().primary() || box.getState().secondary() ||
            box.getState().fatal()) {
            return false;
        }

        minvalid = getMinValid();
        if( minvalid <= lastOpTimeWritten ) {
            golive=true;
        }
        else {
            sethbmsg(str::stream() << "still syncing, not yet to minValid optime " <<
                     minvalid.toString());
        }

        if( golive ) {
            sethbmsg("");
            changeState(MemberState::RS_SECONDARY);
        }
        return golive;
    }


    bool ReplSetImpl::forceSyncFrom(const string& host, string& errmsg, BSONObjBuilder& result) {
        lock lk(this);

        // initial sanity check
        if (iAmArbiterOnly()) {
            errmsg = "arbiters don't sync";
            return false;
        }
        if (box.getState().primary()) {
            errmsg = "primaries don't sync";
            return false;
        }
        if (_self != NULL && host == _self->fullName()) {
            errmsg = "I cannot sync from myself";
            return false;
        }

        // find the member we want to sync from
        Member *newTarget = 0;
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->fullName() == host) {
                newTarget = m;
                break;
            }
        }

        // do some more sanity checks
        if (!newTarget) {
            // this will also catch if someone tries to sync a member from itself, as _self is not
            // included in the _members list.
            errmsg = "could not find member in replica set";
            return false;
        }
        if (newTarget->config().arbiterOnly) {
            errmsg = "I cannot sync from an arbiter";
            return false;
        }
        if (!newTarget->config().buildIndexes && myConfig().buildIndexes) {
            errmsg = "I cannot sync from a member who does not build indexes";
            return false;
        }
        if (newTarget->hbinfo().authIssue) {
            errmsg = "I cannot authenticate against the requested member";
            return false;
        }
        if (newTarget->hbinfo().health == 0) {
            errmsg = "I cannot reach the requested member";
            return false;
        }
        if (newTarget->hbinfo().opTime.getSecs()+10 < lastOpTimeWritten.getSecs()) {
            log() << "attempting to sync from " << newTarget->fullName()
                  << ", but its latest opTime is " << newTarget->hbinfo().opTime.getSecs()
                  << " and ours is " << lastOpTimeWritten.getSecs() << " so this may not work"
                  << rsLog;
            result.append("warning", "requested member is more than 10 seconds behind us");
            // not returning false, just warning
        }

        // record the previous member we were syncing from
        const Member *prev = replset::BackgroundSync::get()->getSyncTarget();
        if (prev) {
            result.append("prevSyncTarget", prev->fullName());
        }

        // finally, set the new target
        _forceSyncTarget = newTarget;
        return true;
    }

    bool ReplSetImpl::gotForceSync() {
        lock lk(this);
        return _forceSyncTarget != 0;
    }

    bool ReplSetImpl::shouldChangeSyncTarget(const OpTime& targetOpTime) const {
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->syncable() &&
                targetOpTime.getSecs()+maxSyncSourceLagSecs < m->hbinfo().opTime.getSecs()) {
                return true;
            }
        }

        return false;
    }

    void ReplSetImpl::_syncThread() {
        StateBox::SP sp = box.get();
        if( sp.state.primary() ) {
            sleepsecs(1);
            return;
        }
        if( _blockSync || sp.state.fatal() || sp.state.startup() ) {
            sleepsecs(5);
            return;
        }

        bool initialSyncRequested = false;
        {
            boost::unique_lock<boost::mutex> lock(theReplSet->initialSyncMutex);
            initialSyncRequested = theReplSet->initialSyncRequested;
        }
        // Check criteria for doing an initial sync:
        // 1. If the oplog is empty, do an initial sync
        // 2. If minValid has _initialSyncFlag set, do an initial sync
        // 3. If initialSyncRequested is true
        if (lastOpTimeWritten.isNull() || getInitialSyncFlag() || initialSyncRequested) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        replset::SyncTail tail(replset::BackgroundSync::get());
        tail.oplogApplication();
    }

    bool ReplSetImpl::resync(string& errmsg) {
        changeState(MemberState::RS_RECOVERING);

        Client::Context ctx("local");
        ctx.db()->dropCollection("local.oplog.rs");
        {
            boost::unique_lock<boost::mutex> lock(theReplSet->initialSyncMutex);
            theReplSet->initialSyncRequested = true;
        }
        lastOpTimeWritten = OpTime();
        _veto.clear();
        return true;
    }

    void ReplSetImpl::syncThread() {
        while( 1 ) {
            // After a reconfig, we may not be in the replica set anymore, so
            // check that we are in the set (and not an arbiter) before
            // trying to sync with other replicas.
            if( ! _self ) {
                log() << "replSet warning did not receive a valid config yet, sleeping 20 seconds " << rsLog;
                sleepsecs(20);
                continue;
            }
            if( myConfig().arbiterOnly ) {
                return;
            }

            fassert(16113, !Lock::isLocked());

            try {
                _syncThread();
            }
            catch(const DBException& e) {
                sethbmsg(str::stream() << "syncThread: " << e.toString());
                sleepsecs(10);
            }
            catch(...) {
                sethbmsg("unexpected exception in syncThread()");
                // TODO : SET NOT SECONDARY here?
                sleepsecs(60);
            }
            sleepsecs(1);
        }
    }

    void startSyncThread() {
        static int n;
        if( n != 0 ) {
            log() << "replSet ERROR : more than one sync thread?" << rsLog;
            verify( n == 0 );
        }
        n++;

        Client::initThread("rsSync");
        replLocalAuth();
        theReplSet->syncThread();
        cc().shutdown();
    }

    void GhostSync::starting() {
        Client::initThread("rsGhostSync");
        replLocalAuth();
    }

    void ReplSetImpl::blockSync(bool block) {
        // RS lock is already taken in Manager::checkAuth
        _blockSync = block;
        if (_blockSync) {
            // syncing is how we get into SECONDARY state, so we'll be stuck in
            // RECOVERING until we unblock
            changeState(MemberState::RS_RECOVERING);
        }
    }

    void GhostSync::clearCache() {
        rwlock lk(_lock, true);
        _ghostCache.clear();
    }

    void GhostSync::associateSlave(const BSONObj& id, const int memberId) {
        const OID rid = id["_id"].OID();
        rwlock lk( _lock , true );
        shared_ptr<GhostSlave> &g = _ghostCache[rid];
        if( g.get() == 0 ) {
            g.reset( new GhostSlave() );
            wassert( _ghostCache.size() < 10000 );
        }
        GhostSlave &slave = *g;
        if (slave.init) {
            LOG(1) << "tracking " << slave.slave->h().toString() << " as " << rid << rsLog;
            return;
        }

        slave.slave = (Member*)rs->findById(memberId);
        if (slave.slave != 0) {
            slave.init = true;
        }
        else {
            log() << "replset couldn't find a slave with id " << memberId
                  << ", not tracking " << rid << rsLog;
        }
    }

    bool GhostSync::updateSlave(const mongo::OID& rid, const OpTime& last) {
        rwlock lk( _lock , false );
        MAP::iterator i = _ghostCache.find( rid );
        if ( i == _ghostCache.end() ) {
            OCCASIONALLY warning() << "couldn't update position of the secondary with replSet _id '"
                                   << rid << "' because we have no entry for it" << rsLog;
            return false;
        }

        GhostSlave& slave = *(i->second);
        if (!slave.init) {
            OCCASIONALLY log() << "couldn't update position of the secondary with replSet _id '"
                               << rid << "' because it has not been initialized" << rsLog;
            return false;
        }

        ((ReplSetConfig::MemberCfg)slave.slave->config()).updateGroups(last);
        return true;
    }

    void GhostSync::percolate(const mongo::OID& rid, const OpTime& last) {
        shared_ptr<GhostSlave> slave;
        {
            rwlock lk( _lock , false );

            MAP::iterator i = _ghostCache.find( rid );
            if ( i == _ghostCache.end() ) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " no entry" << rsLog;
                return;
            }

            slave = i->second;
            if (!slave->init) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " not init" << rsLog;
                return;
            }
        }
        verify(slave->slave);

        // Keep trying to update until we either succeed or we become primary.
        // Note that this can block the ghostsync thread for quite a while if there
        // are connection problems to the current sync source ("sync target")
        while (true) {
            const Member *target = replset::BackgroundSync::get()->getSyncTarget();
            if (!target || rs->box.getState().primary()
                // we are currently syncing from someone who's syncing from us
                // the target might end up with a new Member, but s.slave never
                // changes so we'll compare the names
                || target == slave->slave || target->fullName() == slave->slave->fullName()) {
                LOG(1) << "replica set ghost target no good" << endl;
                return;
            }

            try {
                if (MONGO_FAIL_POINT(rsChaining1)) {
                    mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                        setMode(FailPoint::nTimes, 1);
                }

                // haveCursor() does not necessarily tell us if we have a non-dead cursor, 
                // so we check tailCheck() as well; see SERVER-8420
                slave->reader.tailCheck();
                if (!slave->reader.haveCursor()) {
                    if (!slave->reader.connect(rid, slave->slave->id(), target->fullName())) {
                        // error message logged in OplogReader::connect
                        sleepsecs(1);
                        continue;
                    }

                    if (MONGO_FAIL_POINT(rsChaining2)) {
                        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                            setMode(FailPoint::nTimes, 1);
                    }

                    slave->reader.ghostQueryGTE(rsoplog, last);
                    // if we lose the connection between connecting and querying, the cursor may not
                    // exist so we have to check again before using it.
                    if (!slave->reader.haveCursor()) {
                        sleepsecs(1);
                        continue;
                    }
                }

                LOG(5) << "replSet secondary " << slave->slave->fullName()
                       << " syncing progress updated from " << slave->last.toStringPretty()
                       << " to " << last.toStringPretty() << rsLog;
                if (slave->last > last) {
                    // Nothing to do; already up to date.
                    return;
                }

                while (slave->last <= last) {
                    if (MONGO_FAIL_POINT(rsChaining3)) {
                        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                            setMode(FailPoint::nTimes, 1);
                    }

                    if (!slave->reader.more()) {
                        // Hit the end of the oplog on the sync source; we're fully up to date now.
                        return;
                    }

                    BSONObj o = slave->reader.nextSafe();
                    slave->last = o["ts"]._opTime();
                }
                LOG(2) << "now last is " << slave->last.toString() << rsLog;
                // We moved the cursor forward enough; we're done.
                return;
            }
            catch (const DBException& e) {
                // This captures SocketExceptions as well.
                log() << "replSet ghost sync error: " << e.what() << " for "
                      << slave->slave->fullName() << rsLog;
                slave->reader.resetConnection();
            }
        }
    }
}
