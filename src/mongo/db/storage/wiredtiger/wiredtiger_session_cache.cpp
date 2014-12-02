// wiredtiger_session_cache.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

namespace mongo {

    WiredTigerSession::WiredTigerSession( WT_CONNECTION* conn, int epoch )
        : _epoch( epoch ), _session( NULL ), _cursorsOut( 0 ) {
        int ret = conn->open_session(conn, NULL, "isolation=snapshot", &_session);
        invariantWTOK(ret);
    }

    WiredTigerSession::~WiredTigerSession() {
        if (_session) {
            int ret = _session->close(_session, NULL);
            invariantWTOK(ret);
            _session = NULL;
        }
    }

    WT_CURSOR* WiredTigerSession::getCursor(const std::string& uri, uint64_t id) {
        {
            Cursors& cursors = _curmap[id];
            if ( !cursors.empty() ) {
                WT_CURSOR* save = cursors.back();
                cursors.pop_back();
                _cursorsOut++;
                return save;
            }
        }
        WT_CURSOR* c = NULL;
        int ret = _session->open_cursor(_session, uri.c_str(), NULL, "overwrite=false", &c);
        if (ret != ENOENT)
            invariantWTOK(ret);
        if ( c ) _cursorsOut++;
        return c;
    }

    void WiredTigerSession::releaseCursor(uint64_t id, WT_CURSOR *cursor) {
        invariant( _session );
        invariant( cursor );
        _cursorsOut--;

        Cursors& cursors = _curmap[id];
        if ( cursors.size() > 10u ) {
            invariantWTOK( cursor->close(cursor) );
        }
        else {
            invariantWTOK( cursor->reset( cursor ) );
            cursors.push_back( cursor );
        }
    }

    void WiredTigerSession::closeAllCursors() {
        invariant( _session );
        for (CursorMap::iterator i = _curmap.begin(); i != _curmap.end(); ++i ) {
            Cursors& cursors = i->second;
            for ( size_t j = 0; j < cursors.size(); j++ ) {
                WT_CURSOR *cursor = cursors[j];
                if (cursor) {
                    int ret = cursor->close(cursor);
                    invariantWTOK(ret);
                }
            }
        }
        _curmap.clear();
    }

    namespace {
        AtomicUInt64 nextCursorId(1);
    }
    // static
    uint64_t WiredTigerSession::genCursorId() {
        return nextCursorId.fetchAndAdd(1);
    }

    // -----------------------

    WiredTigerSessionCache::WiredTigerSessionCache( WiredTigerKVEngine* engine )
        : _engine( engine ), _conn( engine->getConnection() ), _shuttingDown(0) {
    }

    WiredTigerSessionCache::WiredTigerSessionCache( WT_CONNECTION* conn )
        : _engine( NULL ), _conn( conn ), _shuttingDown(0) {
    }

    WiredTigerSessionCache::~WiredTigerSessionCache() {
        shuttingDown();
    }

    void WiredTigerSessionCache::shuttingDown() {
        if (_shuttingDown.load()) return;
        _shuttingDown.store(1);

        {
            // This ensures that any calls, which are currently inside of getSession/releaseSession
            // will be able to complete before we start cleaning up the pool. Any others, which are
            // about to enter will return immediately because of _shuttingDown == true.
            boost::lock_guard<boost::shared_mutex> lk(_shutdownLock);
        }

        closeAll();
        invariant(_sessionPool.empty());
    }

    void WiredTigerSessionCache::closeAll() {
        SessionPool swapPool;
        {
            boost::mutex::scoped_lock lk(_sessionLock);
            _sessionPool.swap(swapPool);
        }

        // New sessions will be created if need be outside of the lock
        for (size_t i = 0; i < swapPool.size(); i++) {
            delete swapPool[i];
        }

        swapPool.clear();
    }

    WiredTigerSession* WiredTigerSessionCache::getSession() {
        boost::shared_lock<boost::shared_mutex> shutdownLock(_shutdownLock);

        // We should never be able to get here after _shuttingDown is set, because no new
        // operations should be allowed to start.
        invariant(!_shuttingDown.loadRelaxed());

        {
            boost::mutex::scoped_lock lk( _sessionLock );

            if (!_sessionPool.empty()) {
                WiredTigerSession* cachedSession = _sessionPool.back();
                _sessionPool.pop_back();

                return cachedSession;
            }
        }

        return new WiredTigerSession( _conn, _engine ? _engine->currentEpoch() : -1 );
    }

    void WiredTigerSessionCache::releaseSession( WiredTigerSession* session ) {
        invariant( session );
        invariant(session->cursorsOut() == 0);

        boost::shared_lock<boost::shared_mutex> shutdownLock(_shutdownLock);
        if (_shuttingDown.loadRelaxed()) {
            // Leak the session in order to avoid race condition with clean shutdown, where the
            // storage engine is ripped from underneath transactions, which are not "active"
            // (i.e., do not have any locks), but are just about to delete the recovery unit.
            // See SERVER-16031 for more information.
            return;
        }

        // This checks that we are only caching idle sessions and not something which might hold
        // locks or otherwise prevent truncation.
        {
            WT_SESSION* ss = session->getSession();
            uint64_t range;
            invariantWTOK(ss->transaction_pinned_range(ss, &range));
            invariant(range == 0);
        }

        if (_engine && _engine->haveDropsQueued() && session->epoch() < _engine->currentEpoch()) {
            delete session;
            _engine->dropAllQueued();
            return;
        }

        boost::mutex::scoped_lock lk(_sessionLock);
        _sessionPool.push_back( session );
    }

    bool WiredTigerSessionCache::_shouldBeClosed( WiredTigerSession* session ) const {
        if ( !_engine )
            return false;
        if ( !_engine->haveDropsQueued() )
            return false;
        return session->epoch() < _engine->currentEpoch();
    }
}
