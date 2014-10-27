// wiredtiger_session_cache.h

#pragma once

#include <map>
#include <string>
#include <vector>

#include <boost/thread/mutex.hpp>

#include <wiredtiger.h>

namespace mongo {

    class WiredTigerKVEngine;

    /**
     * This is a structure that caches 1 cursor for each uri.
     * The idea is that there is a pool of these somewhere.
     * NOT THREADSAFE
     */
    class WiredTigerSession {
    public:
        WiredTigerSession( WT_CONNECTION* conn, int epoch );
        ~WiredTigerSession();

        WT_SESSION* getSession() const { return _session; }

        WT_CURSOR* getCursor(const std::string& uri, uint64_t id);
        void releaseCursor(uint64_t id, WT_CURSOR *cursor);

        void closeAllCursors();

        int cursorsOut() const { return _cursorsOut; }

        int epoch() const { return _epoch; }

        static uint64_t genCursorId();

    private:
        int _epoch;
        WT_SESSION* _session; // owned
        typedef std::vector<WT_CURSOR*> Cursors;
        typedef std::map<uint64_t, Cursors> CursorMap;
        CursorMap _curmap; // owned
        int _cursorsOut;
    };

    class WiredTigerSessionCache {
    public:

        WiredTigerSessionCache( WiredTigerKVEngine* engine );
        WiredTigerSessionCache( WT_CONNECTION* conn );
        ~WiredTigerSessionCache();

        WiredTigerSession* getSession();
        void releaseSession( WiredTigerSession* session );

        void closeAll();

    private:

        bool _shouldBeClosed( WiredTigerSession* session ) const;

        void _closeAll(); // does not lock

        WiredTigerKVEngine* _engine; // not owned, might be NULL
        WT_CONNECTION* _conn; // not owned
        typedef std::vector<WiredTigerSession*> SessionPool;
        SessionPool _sessionPool; // owned
        mutable boost::mutex _sessionLock;
    };

}
