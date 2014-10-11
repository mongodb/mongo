// wiredtiger_session_cache.cpp

#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

namespace mongo {

    WiredTigerSession::WiredTigerSession( WT_CONNECTION* conn ) {
        _session = NULL;
        int ret = conn->open_session(conn, NULL, "isolation=read-uncommitted", &_session);
        invariantWTOK(ret);
    }

    WiredTigerSession::~WiredTigerSession() {
        if (_session) {
            int ret = _session->close(_session, NULL);
            invariantWTOK(ret);
            _session = NULL;
        }
    }

    WT_CURSOR* WiredTigerSession::getCursor(const std::string& uri) {
        {
            Cursors& cursors = _curmap[uri];
            if ( !cursors.empty() ) {
                WT_CURSOR* save = cursors.back();
                cursors.pop_back();
                return save;
            }
        }
        WT_CURSOR* c = NULL;
        int ret = _session->open_cursor(_session, uri.c_str(), NULL, NULL, &c);
        if (ret != ENOENT) invariantWTOK(ret);
        return c;
    }

    void WiredTigerSession::releaseCursor(const std::string& uri, WT_CURSOR *cursor) {
        invariant( _session );
        invariant( cursor );

        Cursors& cursors = _curmap[uri];
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
