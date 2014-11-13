// wiredtiger_recovery_unit.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <boost/thread/condition.hpp>
#include <boost/thread/mutex.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

    namespace {
        struct AwaitCommitData {
            AwaitCommitData() :
                numWaitingForSync(0),
                lastSyncTime(0) {
            }

            void syncHappend() {
                boost::mutex::scoped_lock lk( mutex );
                lastSyncTime++;
                condvar.notify_all();
            }

            // return true if happened
            bool awaitCommit() {
                boost::mutex::scoped_lock lk( mutex );
                long long start = lastSyncTime;
                numWaitingForSync.fetchAndAdd(1);
                condvar.timed_wait(lk,boost::posix_time::milliseconds(50));
                numWaitingForSync.fetchAndAdd(-1);
                return lastSyncTime > start;
            }

            AtomicUInt32 numWaitingForSync;

            boost::mutex mutex; // this just protects lastSyncTime
            boost::condition condvar;
            long long lastSyncTime;
        } awaitCommitData;
    }

    WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc) :
        _sessionCache( sc ),
        _session( NULL ),
        _depth(0),
        _active( false ),
        _everStartedWrite( false ),
        _currentlySquirreled( false ),
        _syncing( false ) {
    }

    WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
        invariant( _depth == 0 );
        _abort();
        if ( _session ) {
            _sessionCache->releaseSession( _session );
            _session = NULL;
        }
    }

    void WiredTigerRecoveryUnit::reportState( BSONObjBuilder* b ) const {
        b->append( "wt_depth", _depth );
        b->append( "wt_active", _active );
        b->append( "wt_everStartedWrite", _everStartedWrite );
        if ( _active )
            b->append( "wt_millisSinceCommit", _timer.millis() );
    }

    void WiredTigerRecoveryUnit::_commit() {
        if ( _session && _active ) {
            _txnClose( true );
        }

        for (Changes::iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
        _changes.clear();
    }

    void WiredTigerRecoveryUnit::_abort() {
        if ( _session && _active ) {
            _txnClose( false );
        }

        for (Changes::reverse_iterator it = _changes.rbegin(), end = _changes.rend();
                it != end; ++it) {
            (*it)->rollback();
        }
        _changes.clear();
    }

    void WiredTigerRecoveryUnit::beginUnitOfWork() {
        invariant( !_currentlySquirreled );
        _depth++;
        _everStartedWrite = true;
    }

    void WiredTigerRecoveryUnit::commitUnitOfWork() {
        if (_depth > 1)
            return; // only outermost WUOW gets committed.
        _commit();
    }

    void WiredTigerRecoveryUnit::endUnitOfWork() {
        _depth--;
        if ( _depth == 0 ) {
            _abort();
        }
    }

    void WiredTigerRecoveryUnit::goingToAwaitCommit() {
        if ( _active ) {
            // too late, can't change config
            return;
        }
        // yay, we've configured ourselves for sync
        _syncing = true;
    }

    bool WiredTigerRecoveryUnit::awaitCommit() {
        if ( _syncing && _everStartedWrite ) {
            // we did a sync, so we're good
            return true;
        }
        awaitCommitData.awaitCommit();
        return true;
    }

    void WiredTigerRecoveryUnit::registerChange(Change* change) {
        invariant(_depth > 0);
        _changes.push_back(ChangePtr(change));
    }

    WiredTigerRecoveryUnit* WiredTigerRecoveryUnit::get(OperationContext *txn) {
        invariant( txn );
        return dynamic_cast<WiredTigerRecoveryUnit*>(txn->recoveryUnit());
    }

    WiredTigerSession* WiredTigerRecoveryUnit::getSession() {
        if ( !_session ) {
            _session = _sessionCache->getSession();
        }

        if ( !_active ) {
            _txnOpen();
        }
        return _session;
    }

    void WiredTigerRecoveryUnit::commitAndRestart() {
        invariant( _depth == 0 );
        if ( _active ) {
            _txnClose( true );
        }
    }

    void WiredTigerRecoveryUnit::setOplogReadTill( const DiskLoc& loc ) {
        invariant( _session == NULL || _oplogReadTill == loc );
        _oplogReadTill = loc;
    }

    void WiredTigerRecoveryUnit::_txnClose( bool commit ) {
        invariant( _active );
        WT_SESSION *s = _session->getSession();
        if ( commit ) {
            invariantWTOK( s->commit_transaction(s, NULL) );
            LOG(2) << "WT commit_transaction";
            if ( _syncing )
                awaitCommitData.syncHappend();
        }
        else {
            invariantWTOK( s->rollback_transaction(s, NULL) );
            LOG(2) << "WT rollback_transaction";
        }
        _active = false;
    }

    void WiredTigerRecoveryUnit::_txnOpen() {
        invariant( !_active );
        WT_SESSION *s = _session->getSession();
        _syncing = _syncing || awaitCommitData.numWaitingForSync.load() > 0;
        invariantWTOK( s->begin_transaction(s, _syncing ? "sync=true" : NULL) );
        LOG(2) << "WT begin_transaction";
        _timer.reset();
        _active = true;
    }

    void WiredTigerRecoveryUnit::beingReleasedFromOperationContext() {
        LOG(2) << "WiredTigerRecoveryUnit::beingReleased";
        _currentlySquirreled = true;
        if ( !wt_keeptxnopen() ) {
            _commit();
        }
    }
    void WiredTigerRecoveryUnit::beingSetOnOperationContext() {
        LOG(2) << "WiredTigerRecoveryUnit::broughtBack";
        _currentlySquirreled = false;
    }


    // ---------------------

    WiredTigerCursor::WiredTigerCursor(const std::string& uri, uint64_t id, WiredTigerRecoveryUnit* ru) {
        _init( uri, id, ru );
    }

    WiredTigerCursor::WiredTigerCursor(const std::string& uri, uint64_t id, OperationContext* txn) {
        _init( uri, id, WiredTigerRecoveryUnit::get( txn ) );
    }

    void WiredTigerCursor::_init( const std::string& uri, uint64_t id, WiredTigerRecoveryUnit* ru ) {
        _uriID = id;
        _ru = ru;
        _session = _ru->getSession();
        _cursor = _session->getCursor( uri, id );
        if ( !_cursor ) {
            error() << "no cursor for uri: " << uri;
        }
    }

    WiredTigerCursor::~WiredTigerCursor() {
        invariant( _session == _ru->getSession() );
        _session->releaseCursor( _uriID, _cursor );
        _cursor = NULL;
    }

    WT_CURSOR* WiredTigerCursor::get() const {
        invariant( _session == _ru->getSession() );
        return _cursor;
    }

    void WiredTigerCursor::reset() {
        invariantWTOK( _cursor->reset( _cursor ) );
    }

    WT_SESSION* WiredTigerCursor::getWTSession() {
        return _session->getSession();
    }
}
