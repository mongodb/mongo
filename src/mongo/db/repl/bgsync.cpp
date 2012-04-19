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
 */

#include "mongo/pch.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/client.h"

namespace mongo {
namespace replset {
    BackgroundSync* BackgroundSync::s_instance = 0;
    boost::mutex BackgroundSync::s_mutex;

    BackgroundSyncInterface::~BackgroundSyncInterface() {}

    BackgroundSync::BackgroundSync() : _oplogMarkerTarget(NULL),
                                       _oplogMarker(true /* doHandshake */),
                                       _consume(0, 0) {
    }

    BackgroundSync* BackgroundSync::get() {
        // TODO: SERVER-5112 DCLP is not thread safe
        if (s_instance == NULL) {
            boost::unique_lock<boost::mutex> lock(s_mutex);
            if (s_instance == NULL && !inShutdown()) {
                s_instance = new BackgroundSync();
            }
        }
        return s_instance;
    }

    void BackgroundSync::notifierThread() {
        Client::initThread("rsSyncNotifier");
        replLocalAuth();

        while (!inShutdown()) {
            bool ex = false;

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
                markOplog();
            }
            catch (DBException &e) {
                ex = true;
                log() << "replset tracking exception: " << e.getInfo() << rsLog;
                sleepsecs(1);
            }
            catch (std::exception &e2) {
                ex = true;
                log() << "replset tracking error" << e2.what() << rsLog;
                sleepsecs(10);
            }
            catch (...) {
                ex = true;
                log() << "replset unknown tracking error" << rsLog;
                sleepsecs(20);
            }

            if (ex) {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _oplogMarkerTarget = NULL;
            }
        }

        cc().shutdown();
    }

    void BackgroundSync::markOplog() {
        LOG(3) << "replset markOplog: " << _consume << " " << theReplSet->lastOpTimeWritten << rsLog;

        if (!getCursor()) {
            sleepsecs(1);
            return;
        }

        if (!_oplogMarker.moreInCurrentBatch()) {
            _oplogMarker.more();
        }

        if (!_oplogMarker.more()) {
            _oplogMarker.tailCheck();
            sleepsecs(1);
            return;
        }

        // if this member has written the op at optime T, we want to nextSafe up to and including T
        while (_consume < theReplSet->lastOpTimeWritten && _oplogMarker.more()) {
            BSONObj temp = _oplogMarker.nextSafe();
            _consume = temp["ts"]._opTime();
        }

        // next time through markOplog(), call more() to signal the sync target that we've synced T
    }

    bool BackgroundSync::getCursor() {
        // temp
        Member *_currentSyncTarget = theReplSet->getSyncTarget();

        {
            Lock::GlobalWrite l;
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (!_oplogMarkerTarget || _currentSyncTarget != _oplogMarkerTarget) {
                if (!_currentSyncTarget) {
                    return false;
                }

                log() << "replset setting oplog notifier to " << _currentSyncTarget->fullName() << rsLog;
                _oplogMarkerTarget = _currentSyncTarget;

                _oplogMarker.resetConnection();

                if (!_oplogMarker.connect(_oplogMarkerTarget->fullName())) {
                    LOG(1) << "replset could not connect to " << _oplogMarkerTarget->fullName() << rsLog;
                    _oplogMarkerTarget = NULL;
                    return false;
                }
            }
        }

        if (!_oplogMarker.haveCursor()) {
            BSONObj fields = BSON("ts" << 1);
            _oplogMarker.tailingQueryGTE(rsoplog, theReplSet->lastOpTimeWritten, &fields);
        }

        return _oplogMarker.haveCursor();
    }

} // namespace replset


    bool ReplSetImpl::_isStale(OplogReader& r, const OpTime& startTs, BSONObj& remoteOldestOp) {
        remoteOldestOp = r.findOne(rsoplog, Query());
        OpTime remoteTs = remoteOldestOp["ts"]._opTime();
        DEV log() << "replSet remoteOldestOp:    " << remoteTs.toStringLong() << rsLog;
        else LOG(3) << "replSet remoteOldestOp: " << remoteTs.toStringLong() << rsLog;
        DEV {
            log() << "replSet lastOpTimeWritten: " << lastOpTimeWritten.toStringLong() << rsLog;
            log() << "replSet our state: " << state().toString() << rsLog;
        }
        if( startTs >= remoteTs ) {
            return false;
        }

        return true;
    }

    Member* ReplSetImpl::_getOplogReader(OplogReader& r, const OpTime& minTS) {
        Member *target = 0, *stale = 0;
        BSONObj oldest;

        verify(r.conn() == 0);

        while ((target = getMemberToSyncTo()) != 0) {
            string current = target->fullName();

            if( !r.connect(current) ) {
                log(2) << "replSet can't connect to " << current << " to read operations" << rsLog;
                r.resetConnection();
                veto(current);
                continue;
            }

            if( !minTS.isNull() && _isStale(r, minTS, oldest) ) {
                r.resetConnection();
                veto(current, 600);
                stale = target;
                continue;
            }

            // if we made it here, the target is up and not stale
            return target;
        }

        // the only viable sync target was stale
        if (stale) {
            log() << "replSet error RS102 too stale to catch up, at least from " << stale->fullName() << rsLog;
            log() << "replSet our last optime : " << lastOpTimeWritten.toStringLong() << rsLog;
            log() << "replSet oldest at " << stale->fullName() << " : " << oldest["ts"]._opTime().toStringLong() << rsLog;
            log() << "replSet See http://www.mongodb.org/display/DOCS/Resyncing+a+Very+Stale+Replica+Set+Member" << rsLog;

            // reset minvalid so that we can't become primary prematurely
            {
                Lock::DBWrite lk("local.replset.minvalid");
                Helpers::putSingleton("local.replset.minvalid", oldest);
            }

            sethbmsg("error RS102 too stale to catch up");
            changeState(MemberState::RS_RECOVERING);
            sleepsecs(120);
        }

        return 0;
    }

    bool ReplSetImpl::haveToRollback(OplogReader& r) {
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
                if (theirTS < lastOpTimeWritten) {
                    log() << "replSet we are ahead of the primary, will try to roll back" << rsLog;
                    syncRollback(r);
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
        if( ts != lastOpTimeWritten || h != lastH ) {
            log() << "replSet our last op time written: " << lastOpTimeWritten.toStringPretty() << rsLog;
            log() << "replset source's GTE: " << ts.toStringPretty() << rsLog;
            syncRollback(r);
            return true;
        }

        return false;
    }

    Member* ReplSetImpl::getSyncTarget() {
        lock lk(this);
        return _currentSyncTarget;
    }


} // namespace mongo
