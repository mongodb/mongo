// wiredtiger_session_cache.cpp

#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

namespace mongo {

    WiredTigerSession::WiredTigerSession( WT_CONNECTION* conn ) {
        _session = NULL;
        int ret = conn->open_session(conn, NULL, NULL, &_session);
        invariantWTOK(ret);
    }

    WiredTigerSession::~WiredTigerSession() {
        if (_session) {
            int ret = _session->close(_session, NULL);
            invariantWTOK(ret);
        }
    }

    WT_CURSOR* WiredTigerSession::getCursor(const std::string &uri) {
        {
            WT_CURSOR*& c = _curmap[uri];
            if (c) {
                WT_CURSOR* save = c;
                c = NULL;
                return save;
            }
        }
        WT_CURSOR* c;
        int ret = _session->open_cursor(_session, uri.c_str(), NULL, NULL, &c);
        if (ret != ENOENT) invariantWTOK(ret);
        return c;
    }

    void WiredTigerSession::releaseCursor(WT_CURSOR *cursor) {
        const std::string uri(cursor->uri);
        WT_CURSOR*& old = _curmap[uri];
        if ( old ) {
            // todo: keep vector
            int ret = cursor->close(cursor);
            invariantWTOK(ret);
        }
        else {
            int ret = cursor->reset(cursor);
            invariantWTOK(ret);
            old = cursor;
        }
    }

    void WiredTigerSession::closeAllCursors() {
        for (CursorMap::iterator i = _curmap.begin(); i != _curmap.end(); ++i ) {
            WT_CURSOR *cursor = i->second;
            if (cursor) {
                int ret = cursor->close(cursor);
                invariantWTOK(ret);
            }
        }
        _curmap.clear();
    }


    // -----------------------

    WiredTigerSessionCache::WiredTigerSessionCache( WT_CONNECTION* conn )
        : _conn( conn ) {
    }

    WiredTigerSessionCache::~WiredTigerSessionCache() {
        _closeAll();
    }

    void WiredTigerSessionCache::closeAll() {
        boost::mutex::scoped_lock lk( _sessionLock );
        _closeAll();
    }

    void WiredTigerSessionCache::_closeAll() {
        for ( size_t i = 0; i < _sessionPool.size(); i++ ) {
            delete _sessionPool[i];
        }
        _sessionPool.clear();
    }

    WiredTigerSession* WiredTigerSessionCache::getSession() {
        {
            boost::mutex::scoped_lock lk( _sessionLock );
            if ( !_sessionPool.empty() ) {
                WiredTigerSession* s = _sessionPool.back();
                _sessionPool.pop_back();
                return s;
            }
        }
        return new WiredTigerSession( _conn );
    }

    void WiredTigerSessionCache::releaseSession( WiredTigerSession* session ) {
        invariant( session );
        boost::mutex::scoped_lock lk( _sessionLock );
        _sessionPool.push_back( session );
    }

}
