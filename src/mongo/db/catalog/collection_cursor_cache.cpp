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

#include "mongo/db/catalog/collection_cursor_cache.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/query/runner.h"
#include "mongo/platform/random.h"
#include "mongo/util/startup_test.h"

namespace mongo {

    namespace {
        unsigned idFromCursorId( CursorId id ) {
            uint64_t x = static_cast<uint64_t>(id);
            x = x >> 32;
            return static_cast<unsigned>( x );
        }

        CursorId cursorIdFromParts( unsigned collection,
                                    unsigned cursor ) {
            CursorId x = static_cast<CursorId>( collection ) << 32;
            x |= cursor;
            return x;
        }

        class IdWorkTest : public StartupTest {
        public:
            void _run( unsigned a, unsigned b) {
                CursorId x = cursorIdFromParts( a, b );
                invariant( a == idFromCursorId( x ) );
                CursorId y = cursorIdFromParts( a, b + 1 );
                invariant( x != y );
            }

            void run() {
                _run( 123, 456 );
                _run( 0xdeadbeef, 0xcafecafe );
                _run( 0, 0 );
                _run( 99999999, 999 );
                _run( 0xFFFFFFFF, 1 );
                _run( 0xFFFFFFFF, 0 );
                _run( 0xFFFFFFFF, 0xFFFFFFFF );
            }
        } idWorkTest;
    }

    class GlobalCursorIdCache {
    public:

        GlobalCursorIdCache();
        ~GlobalCursorIdCache();

        /**
         * this gets called when a CollectionCursorCache gets created
         * @return the id the CollectionCursorCache should use when generating
         * cursor ids
         */
        unsigned created( const std::string& ns );

        /**
         * called by CollectionCursorCache when its going away
         */
        void destroyed( unsigned id, const std::string& ns );

        /**
         * works globally
         */
        bool eraseCursor( CursorId id, bool checkAuth );

        void appendStats( BSONObjBuilder& builder );

        std::size_t timeoutCursors( unsigned millisSinceLastCall );

        int64_t nextSeed();

    private:
        SimpleMutex _mutex;

        typedef unordered_map<unsigned,string> Map;
        Map _idToNS;
        unsigned _nextId;

        SecureRandom* _secureRandom;
    } _globalCursorIdCache;

    GlobalCursorIdCache::GlobalCursorIdCache()
        : _mutex( "GlobalCursorIdCache" ),
          _nextId( 0 ),
          _secureRandom( NULL ) {
    }

    GlobalCursorIdCache::~GlobalCursorIdCache() {
        // we're just going to leak everything, as it doesn't matter
    }

    int64_t GlobalCursorIdCache::nextSeed() {
        SimpleMutex::scoped_lock lk( _mutex );
        if ( !_secureRandom )
            _secureRandom = SecureRandom::create();
        return _secureRandom->nextInt64();
    }

    unsigned GlobalCursorIdCache::created( const std::string& ns ) {
        static const unsigned MAX_IDS = 1000 * 1000 * 1000;

        SimpleMutex::scoped_lock lk( _mutex );

        fassert( 17359, _idToNS.size() < MAX_IDS );

        for ( unsigned i = 0; i <= MAX_IDS; i++ ) {
            unsigned id = ++_nextId;
            if ( id == 0 )
                continue;
            if ( _idToNS.count( id ) > 0 )
                continue;
            _idToNS[id] = ns;
            return id;
        }

        invariant( false );
    }

    void GlobalCursorIdCache::destroyed( unsigned id, const std::string& ns ) {
        SimpleMutex::scoped_lock lk( _mutex );
        invariant( ns == _idToNS[id] );
        _idToNS.erase( id );
    }

    bool GlobalCursorIdCache::eraseCursor(CursorId id, bool checkAuth) {
        string ns;
        {
            SimpleMutex::scoped_lock lk( _mutex );
            unsigned nsid = idFromCursorId( id );
            Map::const_iterator it = _idToNS.find( nsid );
            if ( it == _idToNS.end() ) {
                return false;
            }
            ns = it->second;
        }

        NamespaceString nss( ns );

        if ( checkAuth ) {
            AuthorizationSession* as = cc().getAuthorizationSession();
            bool isAuthorized = as->isAuthorizedForActionsOnNamespace(nss,
                                                                      ActionType::killCursors);
            if ( !isAuthorized ) {
                audit::logKillCursorsAuthzCheck( currentClient.get(),
                                                 nss,
                                                 id,
                                                 ErrorCodes::Unauthorized );
                return false;
            }
        }

        Lock::DBRead lock( ns );
        Database* db = dbHolder().get( ns, storageGlobalParams.dbpath );
        if ( !db )
            return false;
        Client::Context context( ns, db );
        Collection* collection = db->getCollection( ns );
        ClientCursor* cursor = NULL;
        if ( collection ) {
            cursor = collection->cursorCache()->find( id );
        }

        if ( !cursor ) {
            if ( checkAuth )
                audit::logKillCursorsAuthzCheck( currentClient.get(),
                                                 nss,
                                                 id,
                                                 ErrorCodes::CursorNotFound );
            return false;
        }
        
        if ( checkAuth )
            audit::logKillCursorsAuthzCheck( currentClient.get(),
                                             nss,
                                             id,
                                             ErrorCodes::OK );

        massert( 16089,
                 str::stream() << "Cannot kill active cursor " << id,
                 cursor->pinValue() < 100 );

        cursor->kill();
        collection->cursorCache()->deregisterCursor( cursor );
        delete cursor;
        return true;
    }

    std::size_t GlobalCursorIdCache::timeoutCursors( unsigned millisSinceLastCall ) {
        vector<string> todo;
        {
            SimpleMutex::scoped_lock lk( _mutex );
            for ( Map::const_iterator i = _idToNS.begin(); i != _idToNS.end(); ++i )
                todo.push_back( i->second );
        }

        size_t totalTimedOut = 0;

        for ( unsigned i = 0; i < todo.size(); i++ ) {
            const string& ns = todo[i];
            Lock::DBRead lock( ns );
            Database* db = dbHolder().get( ns, storageGlobalParams.dbpath );
            if ( !db )
                continue;
            Client::Context context( ns, db );
            Collection* collection = db->getCollection( ns );
            if ( collection == NULL ) {
                continue;
            }

            totalTimedOut += collection->cursorCache()->timeoutCursors( millisSinceLastCall );

        }

        return totalTimedOut;
    }

    // ---

    std::size_t CollectionCursorCache::timeoutCursorsGlobal( unsigned millisSinceLastCall ) {
        return _globalCursorIdCache.timeoutCursors( millisSinceLastCall );
    }

    int CollectionCursorCache::eraseCursorGlobalIfAuthorized(int n, long long* ids) {
        int numDeleted = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( eraseCursorGlobalIfAuthorized( ids[i] ) )
                numDeleted++;
            if ( inShutdown() )
                break;
        }
        return numDeleted;
    }
    bool CollectionCursorCache::eraseCursorGlobalIfAuthorized(CursorId id) {
        return _globalCursorIdCache.eraseCursor( id, true );
    }
    bool CollectionCursorCache::eraseCursorGlobal( CursorId id ) {
        return _globalCursorIdCache.eraseCursor( id, false );
    }


    // --------------------------


    CollectionCursorCache::CollectionCursorCache( const StringData& ns )
        : _ns( ns.toString() ),
          _mutex( "CollectionCursorCache" ) {
        _collectionCacheRuntimeId = _globalCursorIdCache.created( _ns );
        _random.reset( new PseudoRandom( _globalCursorIdCache.nextSeed() ) );
    }

    CollectionCursorCache::~CollectionCursorCache() {
        invalidateAll( true );
        _globalCursorIdCache.destroyed( _collectionCacheRuntimeId, _ns );
    }

    void CollectionCursorCache::invalidateAll( bool collectionGoingAway ) {
        SimpleMutex::scoped_lock lk( _mutex );

        for ( RunnerSet::iterator it = _nonCachedRunners.begin();
              it != _nonCachedRunners.end();
              ++it ) {

            // we kill the runner, but it deletes itself
            Runner* runner = *it;
            runner->kill();
            invariant( runner->collection() == NULL );
        }
        _nonCachedRunners.clear();

        if ( collectionGoingAway ) {
            // we're going to wipe out the world
            for ( CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i ) {
                ClientCursor* cc = i->second;

                cc->kill();

                invariant( cc->getRunner() == NULL || cc->getRunner()->collection() == NULL );

                // If there is a pinValue >= 100, somebody is actively using the CC and we do
                // not delete it.  Instead we notify the holder that we killed it.  The holder
                // will then delete the CC.
                // pinvalue is <100, so there is nobody actively holding the CC.  We can
                // safely delete it as nobody is holding the CC.

                if (cc->pinValue() < 100) {
                    delete cc;
                }
            }
        }
        else {
            CursorMap newMap;

            // collection will still be around, just all Runners are invalid
            for ( CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i ) {
                ClientCursor* cc = i->second;

                if (cc->isAggCursor) {
                    // Aggregation cursors don't have their lifetime bound to the underlying collection.
                    newMap[i->first] = i->second;
                    continue;
                }

                // Note that a valid ClientCursor state is "no cursor no runner."  This is because
                // the set of active cursor IDs in ClientCursor is used as representation of query
                // state.  See sharding_block.h.  TODO(greg,hk): Move this out.
                if (NULL == cc->getRunner() ) {
                    newMap.insert( *i );
                    continue;
                }

                if (cc->pinValue() < 100) {
                    cc->kill();
                    delete cc;
                }
                else {
                    // this is pinned, so still alive, so we leave around
                    // we kill the Runner to signal
                    if ( cc->getRunner() )
                        cc->getRunner()->kill();
                    newMap.insert( *i );
                }

            }

            _cursors = newMap;
        }
    }

    void CollectionCursorCache::invalidateDocument( const DiskLoc& dl,
                                                    InvalidationType type ) {
        SimpleMutex::scoped_lock lk( _mutex );

        for ( RunnerSet::iterator it = _nonCachedRunners.begin();
              it != _nonCachedRunners.end();
              ++it ) {

            Runner* runner = *it;
            runner->invalidate(dl, type);
        }

        for ( CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i ) {
            Runner* runner = i->second->getRunner();
            if ( runner ) {
                runner->invalidate(dl, type);
            }
        }
    }

    std::size_t CollectionCursorCache::timeoutCursors( unsigned millisSinceLastCall ) {
        SimpleMutex::scoped_lock lk( _mutex );

        vector<ClientCursor*> toDelete;

        for ( CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i ) {
            ClientCursor* cc = i->second;
            if ( cc->shouldTimeout( millisSinceLastCall ) )
                toDelete.push_back( cc );
        }

        for ( vector<ClientCursor*>::const_iterator i = toDelete.begin(); i != toDelete.end(); ++i ) {
            ClientCursor* cc = *i;
            _deregisterCursor_inlock( cc );
            cc->kill();
            delete cc;
        }

        return toDelete.size();
    }

    void CollectionCursorCache::registerRunner( Runner* runner ) {
        SimpleMutex::scoped_lock lk( _mutex );
        const std::pair<RunnerSet::iterator, bool> result = _nonCachedRunners.insert(runner);
        invariant(result.second); // make sure this was inserted
    }

    void CollectionCursorCache::deregisterRunner( Runner* runner ) {
        SimpleMutex::scoped_lock lk( _mutex );
        _nonCachedRunners.erase( runner );
    }

    ClientCursor* CollectionCursorCache::find( CursorId id ) {
        SimpleMutex::scoped_lock lk( _mutex );
        CursorMap::const_iterator it = _cursors.find( id );
        if ( it == _cursors.end() )
            return NULL;
        return it->second;
    }

    void CollectionCursorCache::getCursorIds( std::set<CursorId>* openCursors ) {
        SimpleMutex::scoped_lock lk( _mutex );

        for ( CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i ) {
            ClientCursor* cc = i->second;
            openCursors->insert( cc->cursorid() );
        }
    }

    size_t CollectionCursorCache::numCursors(){
        SimpleMutex::scoped_lock lk( _mutex );
        return _cursors.size();
    }

    CursorId CollectionCursorCache::_allocateCursorId_inlock() {
        for ( int i = 0; i < 10000; i++ ) {
            unsigned mypart = static_cast<unsigned>( _random->nextInt32() );
            CursorId id = cursorIdFromParts( _collectionCacheRuntimeId, mypart );
            if ( _cursors.count( id ) == 0 )
                return id;
        }
        fassertFailed( 17360 );
    }

    CursorId CollectionCursorCache::registerCursor( ClientCursor* cc ) {
        invariant( cc );
        SimpleMutex::scoped_lock lk( _mutex );
        CursorId id = _allocateCursorId_inlock();
        _cursors[id] = cc;
        return id;
    }

    void CollectionCursorCache::deregisterCursor( ClientCursor* cc ) {
        SimpleMutex::scoped_lock lk( _mutex );
        _deregisterCursor_inlock( cc );
    }

    void CollectionCursorCache::_deregisterCursor_inlock( ClientCursor* cc ) {
        invariant( cc );
        CursorId id = cc->cursorid();
        _cursors.erase( id );
    }

}
