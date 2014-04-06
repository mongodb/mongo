// collection_cursor_cache.h

/**
*    Copyright (C) 2013 MongoDB Inc.
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

#pragma once

#include "mongo/db/clientcursor.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/invalidation_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

    class PseudoRandom;
    class Runner;

    class CollectionCursorCache {
    public:
        CollectionCursorCache( const StringData& ns );

        /**
         * will kill() all Runner instances it has
         */
        ~CollectionCursorCache();

        // -----------------

        /**
         * @param collectionGoingAway should be tru if the Collection instance is going away
         *                            this could be because the db is being closed, or the
         *                            collection/db is being dropped.
         */
        void invalidateAll( bool collectionGoingAway );

        /**
         * Broadcast a document invalidation to all relevant Runner(s).  invalidateDocument must
         * called *before* the provided DiskLoc is about to be deleted or mutated.
         */
        void invalidateDocument( const DiskLoc& dl,
                                 InvalidationType type );

        /*
         * timesout cursors that have been idle for too long
         * note: must have a readlock on the collection
         * @return number timed out
         */
        std::size_t timeoutCursors( unsigned millisSinceLastCall );

        // -----------------

        /**
         * Register a runner so that it can be notified of deletion/invalidation during yields.
         * Must be called before a runner yields.  If a runner is cached (inside a ClientCursor) it
         * MUST NOT be registered; the two are mutually exclusive.
         */
        void registerRunner(Runner* runner);

        /**
         * Remove a runner from the runner registry.
         */
        void deregisterRunner(Runner* runner);

        // -----------------

        CursorId registerCursor( ClientCursor* cc );
        void deregisterCursor( ClientCursor* cc );

        bool eraseCursor( CursorId id, bool checkAuth );

        void getCursorIds( std::set<CursorId>* openCursors );
        std::size_t numCursors();

        /**
         * @param pin - if true, will try to pin cursor
         *                  if pinned already, will assert
         *                  otherwise will pin
         */
        ClientCursor* find( CursorId id, bool pin );

        void unpin( ClientCursor* cursor );

        // ----------------------

        static int eraseCursorGlobalIfAuthorized( int n, long long* ids );
        static bool eraseCursorGlobalIfAuthorized( CursorId id );

        static bool eraseCursorGlobal( CursorId id );

        /**
         * @return number timed out
         */
        static std::size_t timeoutCursorsGlobal( unsigned millisSinceLastCall );

    private:
        CursorId _allocateCursorId_inlock();
        void _deregisterCursor_inlock( ClientCursor* cc );

        NamespaceString _nss;
        unsigned _collectionCacheRuntimeId;
        scoped_ptr<PseudoRandom> _random;

        SimpleMutex _mutex;

        typedef unordered_set<Runner*> RunnerSet;
        RunnerSet _nonCachedRunners;

        typedef std::map<CursorId,ClientCursor*> CursorMap;
        CursorMap _cursors;
    };

}
