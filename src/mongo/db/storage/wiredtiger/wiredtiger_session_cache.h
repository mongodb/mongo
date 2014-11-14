// wiredtiger_session_cache.h

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

#pragma once

#include <map>
#include <string>
#include <vector>

#include <boost/thread/mutex.hpp>
#include <boost/thread/shared_mutex.hpp>

#include <wiredtiger.h>

#include "mongo/platform/atomic_word.h"

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

        /**
         * For "metadata:" cursors. Guaranteed never to collide with genCursorId() ids.
         */
        static const uint64_t kMetadataCursorId = 0;

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

        void shuttingDown();

    private:
        typedef std::vector<WiredTigerSession*> SessionPool;


        bool _shouldBeClosed( WiredTigerSession* session ) const;


        WiredTigerKVEngine* _engine; // not owned, might be NULL
        WT_CONNECTION* _conn; // not owned

        // Session pool and protection around the get/return/cleanup methods
        mutable boost::mutex _sessionLock;
        SessionPool _sessionPool;

        // Regular operations take it in shared mode. Shutdown sets the _shuttingDown flag and
        // then takes it in exclusive mode. This ensures that all threads, which would return
        // sessions to the cache would leak them.
        boost::shared_mutex _shutdownLock;
        AtomicUInt32 _shuttingDown; // Used as boolean - 0 = false, 1 = true
    };

}
