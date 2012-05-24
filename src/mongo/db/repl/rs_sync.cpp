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
*/

#include "pch.h"
#include "mongo/db/client.h"
#include "rs.h"
#include "mongo/db/repl.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/bgsync.h"

namespace mongo {

    using namespace bson;
    extern unsigned replSetForceInitialSyncFailure;

    replset::SyncTail::SyncTail(BackgroundSyncInterface *q) : Sync(""), _queue(q) {}

    replset::SyncTail::~SyncTail() {}

    BSONObj* replset::SyncTail::peek() {
        return _queue->peek();
    }

    void replset::SyncTail::consume() {
        _queue->consume();
    }

    replset::InitialSync::InitialSync(BackgroundSyncInterface *q) : SyncTail(q) {}

    replset::InitialSync::~InitialSync() {}

    void NOINLINE_DECL blank(const BSONObj& o) {
        if( *o.getStringField("op") != 'n' ) {
            log() << "replSet skipping bad op in oplog: " << o.toString() << rsLog;
        }
    }

    /* apply the log op that is in param o
       @return bool success (true) or failure (false)
    */
    bool replset::SyncTail::syncApply(const BSONObj &o) {
        const char *ns = o.getStringField("ns");
        if ( *ns == '.' || *ns == 0 ) {
            blank(o);
            return true;
        }

        Client::Context ctx(ns);
        ctx.getClient()->curop()->reset();
        return !applyOperation_inlock(o);
    }

    /* initial oplog application, during initial sync, after cloning.
       @return false on failure.
       this method returns an error and doesn't throw exceptions (i think).
    */
    bool replset::InitialSync::oplogApplication(const BSONObj& applyGTEObj, const BSONObj& minValidObj) {
        OpTime applyGTE = applyGTEObj["ts"]._opTime();
        OpTime minValid = minValidObj["ts"]._opTime();

        if (replSetForceInitialSyncFailure > 0) {
            log() << "replSet test code invoked, forced InitialSync failure: " << replSetForceInitialSyncFailure << rsLog;
            replSetForceInitialSyncFailure--;
            throw DBException("forced error",0);
        }

        /* we lock outside the loop to avoid the overhead of locking on every operation. */
        Lock::GlobalWrite lk;

        applyOp(applyGTEObj);

        // if there were no writes during the initial sync, there will be nothing in the queue so
        // just go live
        if (minValid == applyGTE) {
            return true;
        }

        OpTime ts;
        time_t start = time(0);
        unsigned long long n = 0;
        int fails = 0;
        while( ts < minValid ) {
            try {
                BSONObj* o = peek();

                if (!o) {
                    OCCASIONALLY log() << "replSet initial sync oplog: no more records" << endl;
                    if (fails++ > 30) {
                        log() << "replSet initial sync couldn't get records for 30 seconds, giving up" << endl;
                        log() << "ts: " << ts << " minValid: " << minValid << endl;
                        return false;
                    }

                    sleepsecs(1);
                    continue;
                }
                fails = 0;

                ts = (*o)["ts"]._opTime();
                applyOp(*o);
                consume();

                if ( ++n % 1000 == 0 ) {
                    time_t now = time(0);
                    if (now - start > 10) {
                        // simple progress metering
                        log() << "replSet initialSyncOplogApplication applied " << n << " operations, synced to "
                              << ts.toStringPretty() << rsLog;
                        start = now;
                    }
                }

                getDur().commitIfNeeded();
            }
            catch (DBException& e) {
                // Skip duplicate key exceptions.
                // These are relatively common on initial sync: if a document is inserted
                // early in the clone step, the insert will be replayed but the document
                // will probably already have been cloned over.
                if( e.getCode() == 11000 || e.getCode() == 11001 || e.getCode() == 12582) {
                    continue;
                }

                if( ts <= minValid ) {
                    // didn't make it far enough
                    log() << "replSet initial sync failing, error applying oplog : " << e.toString() << rsLog;
                    return false;
                }

                // otherwise, whatever, we'll break out of the loop and catch
                // anything that's really wrong in syncTail
            }
        }
        return true;
    }

    void replset::InitialSync::applyOp(const BSONObj& o) {

        // optimes before we started copying need not be applied.
        if (!syncApply(o)) {
            if (shouldRetry(o)) {
                uassert(15915, "replSet update still fails after adding missing object", syncApply(o));
            }
        }

        // with repl sets we write the ops to our oplog, too
        _logOpObjRS(o);
    }

    /* should be in RECOVERING state on arrival here.
       readlocks
       @return true if transitioned to SECONDARY
    */
    bool ReplSetImpl::tryToGoLiveAsASecondary(OpTime& /*out*/ minvalid) {
        bool golive = false;

        // make sure we're not primary or secondary already
        if (box.getState().primary() || box.getState().secondary()) {
            return false;
        }

        {
            lock lk( this );

            if (_maintenanceMode > 0) {
                // we're not actually going live
                return true;
            }
        }

        {
            Lock::DBRead lk("local.replset.minvalid");
            BSONObj mv;
            if( Helpers::getSingleton("local.replset.minvalid", mv) ) {
                minvalid = mv["ts"]._opTime();
                if( minvalid <= lastOpTimeWritten ) {
                    golive=true;
                }
                else {
                    sethbmsg(str::stream() << "still syncing, not yet to minValid optime " << minvalid.toString());
                }
            }
            else
                golive = true; /* must have been the original member */
        }
        if( golive ) {
            sethbmsg("");
            changeState(MemberState::RS_SECONDARY);
        }
        return golive;
    }

    extern SimpleMutex filesLockedFsync;

    class DontLockOnEverySingleOperation : boost::noncopyable { 
        scoped_ptr<Lock::ScopedLock> lk;
        scoped_ptr<SimpleMutex::scoped_lock> fsync;
    public:
        void reset() {
            lk.reset();
            fsync.reset();
        }
        void reset(const char *ns) { 
            reset();
            verify( !Lock::isLocked() );
            fsync.reset( new SimpleMutex::scoped_lock(filesLockedFsync) );
            if( ns == 0 ) {
                lk.reset( new Lock::GlobalWrite() );
            }
            else {
                lk.reset( new Lock::DBWrite(ns) );
            }
        }
    };

    /* tail an oplog.  ok to return, will be re-called. */
    void replset::SyncTail::oplogApplication() {
        while( 1 ) {
            verify( !Lock::isLocked() );
            {
                int count = 0;
                Timer timeInWriteLock;
                DontLockOnEverySingleOperation lk;
                while( 1 ) {
                    // occasionally check some things
                    if (count-- <= 0 || time(0) % 10 == 0) {
                        if (theReplSet->isPrimary()) {
                            return;
                        }

                        // can we become secondary?
                        // we have to check this before calling mgr, as we must be a secondary to
                        // become primary
                        if (!theReplSet->isSecondary()) {
                            OpTime minvalid;
                            theReplSet->tryToGoLiveAsASecondary(minvalid);
                        }

                        // normally msgCheckNewState gets called periodically, but in a single node repl set
                        // there are no heartbeat threads, so we do it here to be sure.  this is relevant if the
                        // singleton member has done a stepDown() and needs to come back up.
                        if (theReplSet->config().members.size() == 1 &&
                            theReplSet->myConfig().potentiallyHot()) {
                            theReplSet->mgr->send(boost::bind(&Manager::msgCheckNewState, theReplSet->mgr));
                            sleepsecs(1);
                            return;
                        }

                        count = 200;
                    }

                    if( timeInWriteLock.micros() > 1000 ) {
                        lk.reset();
                        timeInWriteLock.reset();
                    }

                    {
                        const BSONObj *next = peek();

                        if (next == NULL) {
                            bool golive = false;

                            if (!theReplSet->isSecondary()) {
                                OpTime minvalid;
                                golive = theReplSet->tryToGoLiveAsASecondary(minvalid);
                            }

                            if (!golive) {
                                lk.reset();
                                timeInWriteLock.reset();
                                sleepsecs(1);
                            }

                            break;
                        }

                        const BSONObj& o = *next;

                        int sd = theReplSet->myConfig().slaveDelay;

                        // ignore slaveDelay if the box is still initializing. once
                        // it becomes secondary we can worry about it.
                        if( sd && theReplSet->isSecondary() ) {
                            const OpTime ts = o["ts"]._opTime();
                            long long a = ts.getSecs();
                            long long b = time(0);
                            long long lag = b - a;
                            long long sleeptime = sd - lag;
                            if( sleeptime > 0 ) {
                                lk.reset();
                                uassert(12000, "rs slaveDelay differential too big check clocks and systems", sleeptime < 0x40000000);
                                if( sleeptime < 60 ) {
                                    sleepsecs((int) sleeptime);
                                }
                                else {
                                    log() << "replSet slavedelay sleep long time: " << sleeptime << rsLog;
                                    // sleep(hours) would prevent reconfigs from taking effect & such!
                                    long long waitUntil = b + sleeptime;
                                    while( 1 ) {
                                        sleepsecs(6);
                                        if( time(0) >= waitUntil )
                                            break;

                                        if( theReplSet->myConfig().slaveDelay != sd ) // reconf
                                            break;
                                    }
                                }
                            }
                        } // endif slaveDelay

                        const char *ns = o.getStringField("ns");
                        if( ns ) {
                            if ( strlen(ns) == 0 ) {
                                // this is ugly
                                // this is often a no-op
                                // but can't be 100% sure
                                lk.reset();
                                verify( !Lock::isLocked() );
                                lk.reset(0);
                            }
                            else if( str::contains(ns, ".$cmd") ) {
                                // a command may need a global write lock. so we will conservatively go ahead and grab one here. suboptimal. :-(
                                lk.reset();
                                verify( !Lock::isLocked() );
                                lk.reset(0);
                            }
                            else if( !Lock::isWriteLocked(ns) || Lock::isW() ) {
                                // we don't relock on every single op to try to be faster. however if switching collections, we have to.
                                // note here we must reset to 0 first to assure the old object is destructed before our new operator invocation.
                                lk.reset();
                                verify( !Lock::isLocked() );
                                lk.reset(ns);
                            }
                        }

                        try {
                            /* if we have become primary, we dont' want to apply things from elsewhere
                               anymore. assumePrimary is in the db lock so we are safe as long as
                               we check after we locked above. */
                            if( theReplSet->isPrimary() ) {
                                log(0) << "replSet stopping syncTail we are now primary" << rsLog;
                                return;
                            }

                            syncApply(o);
                            _logOpObjRS(o);   // with repl sets we write the ops to our oplog too
                            getDur().commitIfNeeded();

                            // we don't want the catch to reference next after it's been freed
                            next = NULL;
                            consume();
                        }
                        catch (DBException& e) {
                            sethbmsg(str::stream() << "syncTail: " << e.toString());
                            if (next) {
                                log() << "syncing: " << next->toString() << endl;
                            }
                            lk.reset();
                            sleepsecs(30);
                            return;
                        }
                    }
                } // end while
            } // end writelock scope
        }
    }

    bool ReplSetImpl::forceSyncFrom(const string& host, string& errmsg, BSONObjBuilder& result) {
        lock lk(this);

        // initial sanity check
        if (iAmArbiterOnly()) {
            errmsg = "arbiters don't sync";
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
        Member *prev = replset::BackgroundSync::get()->getSyncTarget();
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

        /* do we have anything at all? */
        if( lastOpTimeWritten.isNull() ) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        replset::SyncTail tail(replset::BackgroundSync::get());
        tail.oplogApplication();
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
            catch(DBException& e) {
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
        cc().iAmSyncThread(); // for isSyncThread() (which is used not used much, is used in secondary create index code
        replLocalAuth();
        theReplSet->syncThread();
        cc().shutdown();
    }

    void GhostSync::starting() {
        Client::initThread("rsGhostSync");
        replLocalAuth();
    }

    void ReplSetImpl::blockSync(bool block) {
        _blockSync = block;
        if (_blockSync) {
            // syncing is how we get into SECONDARY state, so we'll be stuck in
            // RECOVERING until we unblock
            changeState(MemberState::RS_RECOVERING);
        }
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

    void GhostSync::updateSlave(const mongo::OID& rid, const OpTime& last) {
        rwlock lk( _lock , false );
        MAP::iterator i = _ghostCache.find( rid );
        if ( i == _ghostCache.end() ) {
            OCCASIONALLY warning() << "couldn't update slave " << rid << " no entry" << rsLog;
            return;
        }

        GhostSlave& slave = *(i->second);
        if (!slave.init) {
            OCCASIONALLY log() << "couldn't update slave " << rid << " not init" << rsLog;
            return;
        }

        ((ReplSetConfig::MemberCfg)slave.slave->config()).updateGroups(last);
    }

    void GhostSync::percolate(const BSONObj& id, const OpTime& last) {
        const OID rid = id["_id"].OID();
        GhostSlave* slave;
        {
            rwlock lk( _lock , false );

            MAP::iterator i = _ghostCache.find( rid );
            if ( i == _ghostCache.end() ) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " no entry" << rsLog;
                return;
            }

            slave = i->second.get();
            if (!slave->init) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " not init" << rsLog;
                return;
            }
        }

        verify(slave->slave);

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
            if (!slave->reader.haveCursor()) {
                if (!slave->reader.connect(id, slave->slave->id(), target->fullName())) {
                    // error message logged in OplogReader::connect
                    return;
                }
                slave->reader.ghostQueryGTE(rsoplog, last);
            }

            LOG(1) << "replSet last: " << slave->last.toString() << " to " << last.toString() << rsLog;
            if (slave->last > last) {
                return;
            }

            while (slave->last <= last) {
                if (!slave->reader.more()) {
                    // we'll be back
                    return;
                }

                BSONObj o = slave->reader.nextSafe();
                slave->last = o["ts"]._opTime();
            }
            LOG(2) << "now last is " << slave->last.toString() << rsLog;
        }
        catch (DBException& e) {
            // we'll be back
            LOG(2) << "replSet ghost sync error: " << e.what() << " for "
                   << slave->slave->fullName() << rsLog;
            slave->reader.resetConnection();
        }
    }
}
