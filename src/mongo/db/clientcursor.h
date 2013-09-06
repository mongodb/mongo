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

#include "mongo/pch.h"

#include <boost/thread/recursive_mutex.hpp>

#include "mongo/db/cc_by_loc.h"
#include "mongo/db/cursor.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher.h"
#include "mongo/db/projection.h"
#include "mongo/db/query/runner.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/util/net/message.h"
#include "mongo/util/background.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

    typedef boost::recursive_mutex::scoped_lock recursive_scoped_lock;
    class ClientCursor;
    class ParsedQuery;

    /**
     * ClientCursor is a wrapper that represents a cursorid from our database application's
     * perspective.
     */
    class ClientCursor : private boost::noncopyable {
    public:
        ClientCursor(int qopts, const shared_ptr<Cursor>& c, const string& ns,
                     BSONObj query = BSONObj());

        ClientCursor(Runner* runner, int qopts = 0, const BSONObj query = BSONObj());

        ~ClientCursor();

        /**
         * Assert that there are no open cursors.
         * Called from DatabaseHolder::closeAll.
         */
        static void assertNoCursors();

        //
        // Basic accessors
        //

        CursorId cursorid() const { return _cursorid; }
        string ns() const { return _ns; }
        Database * db() const { return _db; }

        //
        // Invalidation of DiskLocs and dropping of namespaces
        //

        /**
         * Get rid of cursors for namespaces 'ns'. When dropping a db, ns is "dbname." Used by drop,
         * dropIndexes, dropDatabase.
         */
        static void invalidate(const char *ns);

        /**
         * Called when the provided DiskLoc is about to change state via a deletion or an update.
         * All runners/cursors that might be using that DiskLoc must adapt.
         */
        static void aboutToDelete(const StringData& ns,
                                  const NamespaceDetails* nsd,
                                  const DiskLoc& dl);

        /**
         * Register a runner so that it can be notified of deletion/invalidation during yields.
         * Must be called before a runner yields.  If a runner is cached (inside a ClientCursor) it
         * MUST NOT be registered; the two are mutually exclusive.
         */
        static void registerRunner(Runner* runner);

        /**
         * Remove a runner from the runner registry.
         */
        static void deregisterRunner(Runner* runner);

        //
        // Yielding.
        // 

        static void staticYield(int micros, const StringData& ns, Record* rec);

        //
        // Static methods about all ClientCursors  TODO: Document.
        //

        static void appendStats( BSONObjBuilder& result );

        //
        // ClientCursor creation/deletion.
        //

        static unsigned numCursors() { return clientCursorsById.size(); }
        static void find( const string& ns , set<CursorId>& all );
        static ClientCursor* find(CursorId id, bool warn = true);

        // Same as erase but checks to make sure this thread has read permission on the cursor's
        // namespace.  This should be called when receiving killCursors from a client.  This should
        // not be called when ccmutex is held.
        static int eraseIfAuthorized(int n, long long* ids);
        static bool eraseIfAuthorized(CursorId id);

        /**
         * @return number of cursors found
         */
        static int erase(int n, long long* ids);

        /**
         * Deletes the cursor with the provided @param 'id' if one exists.
         * @throw if the cursor with the provided id is pinned.
         * This does not do any auth checking and should be used only when erasing cursors as part
         * of cleaning up internal operations.
         */
        static bool erase(CursorId id);

        //
        // Timing and timeouts
        //

        /**
         * called every 4 seconds.  millis is amount of idle time passed since the last call --
         * could be zero
         */
        static void idleTimeReport(unsigned millis);

        /**
         * @param millis amount of idle passed time since last call
         * note called outside of locks (other than ccmutex) so care must be exercised
         */
        bool shouldTimeout( unsigned millis );
        unsigned idleTime() const { return _idleAgeMillis; }

        uint64_t getLeftoverMaxTimeMicros() const { return _leftoverMaxTimeMicros; }
        void setLeftoverMaxTimeMicros( uint64_t leftoverMaxTimeMicros ) {
            _leftoverMaxTimeMicros = leftoverMaxTimeMicros;
        }

        //
        // Sharding-specific data.  TODO: Document.
        //

        // future getMore.
        void setCollMetadata( CollectionMetadataPtr metadata ){ _collMetadata = metadata; }
        CollectionMetadataPtr getCollMetadata(){ return _collMetadata; }

        //
        // Replication-related stuff.  TODO: Document and clean.
        //

        void updateSlaveLocation( CurOp& curop );
        void slaveReadTill( const OpTime& t ) { _slaveReadTill = t; }
        /** Just for testing. */
        OpTime getSlaveReadTill() const { return _slaveReadTill; }

        //
        // Query-specific functionality that may be adapted for the Runner.
        //

        Runner* getRunner() const { return _runner.get(); }
        int queryOptions() const { return _queryOptions; }

        // Used by ops/query.cpp to stash how many results have been returned by a query.
        int pos() const { return _pos; }
        void incPos(int n) { _pos += n; }
        void setPos(int n) { _pos = n; }

        //
        // Yielding that is DEPRECATED.  Will be removed when we use runners and they yield
        // internally.
        //

        /**
         * DEPRECATED
         * @param microsToSleep -1 : ask client
         *                       0 : pthread_yield or equivilant
         *                      >0 : sleep for that amount
         * @param recordToLoad after yielding lock, load this record with only mmutex
         * do a dbtemprelease
         * note: caller should check matcher.docMatcher().atomic() first and not yield if atomic -
         *       we don't do herein as this->matcher (above) is only initialized for true queries/getmore.
         *       (ie not set for remote/update)
         * @return if the cursor is still valid.
         *         if false is returned, then this ClientCursor should be considered deleted -
         *         in fact, the whole database could be gone.
         */
        bool yield( int microsToSleep = -1, Record * recordToLoad = 0 );

        enum RecordNeeds {
            DontNeed = -1 , MaybeCovered = 0 , WillNeed = 100
        };
            
        /**
         * @param needRecord whether or not the next record has to be read from disk for sure
         *                   if this is true, will yield of next record isn't in memory
         * @param yielded true if a yield occurred, and potentially if a yield did not occur
         * @return same as yield()
         */
        bool yieldSometimes( RecordNeeds need, bool *yielded = 0 );
        struct YieldData { CursorId _id; bool _doingDeletes; };
        bool prepareToYield( YieldData &data );
        static bool recoverFromYield( const YieldData &data );
        static int suggestYieldMicros();

        //
        // Cursor-only DEPRECATED methods.
        //

        void storeOpForSlave( DiskLoc last );

        // Only used by ops/query.cpp, which will stop using them when queries are answered only by
        // a runner.
        const BSONObj& query() const { return _query; }
        shared_ptr<ParsedQuery> pq;
        // This one is used also by pipeline/document_source_cursor.cpp
        shared_ptr<Projection> fields; // which fields query wants returned

        DiskLoc lastLoc() const { return _lastLoc; }
        Cursor* c() const { return _c.get(); }
        bool ok() { return _c->ok(); }
        bool advance() { return _c->advance(); }
        BSONObj current() { return _c->current(); }
        DiskLoc currLoc() { return _c->currLoc(); }
        BSONObj currKey() const { return _c->currKey(); }

        bool currentIsDup() { return _c->getsetdup( _c->currLoc() ); }
        bool currentMatches() {
            if ( ! _c->matcher() )
                return true;
            return _c->matcher()->matchesCurrent( _c.get() );
        }

        void setDoingDeletes( bool doingDeletes ) {_doingDeletes = doingDeletes; }

    private:
        friend class ClientCursorHolder;
        friend class ClientCursorPin;
        friend struct ClientCursorYieldLock;
        friend class CmdCursorInfo;

        // A map from the CursorId to the ClientCursor behind it.
        // TODO: Consider making this per-connection.
        typedef map<CursorId, ClientCursor*> CCById;
        static CCById clientCursorsById;

        // A list of NON-CACHED runners.  Any runner that yields must be put into this map before
        // yielding in order to be notified of invalidation and namespace deletion.  Before the
        // runner is deleted, it must be removed from this map.
        //
        // TODO: This is temporary and as such is highly NOT optimized.
        static set<Runner*> nonCachedRunners;

        // How many cursors have timed out?
        static long long numberTimedOut;

        // This must be held when modifying any static member.
        static boost::recursive_mutex& ccmutex;

        /**
         * Initialization common between Cursor and Runner.
         * TODO: Remove when we're all-runner.
         */
        void init();

        /**
         * Allocates a new CursorId.
         * Called from init(...).  Assumes ccmutex held.
         */
        static CursorId allocCursorId_inlock();

        /**
         * Find the ClientCursor with the provided ID.  Optionally warn if it's not found.
         * Assumes ccmutex is held.
         */

        static ClientCursor* find_inlock(CursorId id, bool warn = true);

        /**
         * Delete the ClientCursor with the provided ID.  masserts if the cursor is pinned.
         */
        static void _erase_inlock(ClientCursor* cursor);

        //
        // ClientCursor-specific data, independent of the underlying execution type.
        //

        // The ID of the ClientCursor.
        CursorId _cursorid;

        // A variable indicating the state of the ClientCursor.  Possible values:
        //   0: Normal behavior.  May time out.
        //   1: No timing out of this ClientCursor.
        // 100: Currently in use (via ClientCursorPin).
        unsigned _pinValue;

        // The namespace we're operating on.
        string _ns;

        // The database we're operating on.
        Database* _db;

        // How many objects have been returned by the find() so far?
        int _pos;

        // The query that prompted this ClientCursor.  Only used for debugging.
        BSONObj _query;

        // See the QueryOptions enum in dbclient.h
        int _queryOptions;

        // TODO: document better.
        OpTime _slaveReadTill;

        // How long has the cursor been idle?
        unsigned _idleAgeMillis;

        // TODO: Document.
        uint64_t _leftoverMaxTimeMicros;

        // For chunks that are being migrated, there is a period of time when that chunks data is in
        // two shards, the donor and the receiver one. That data is picked up by a cursor on the
        // receiver side, even before the migration was decided.  The CollectionMetadata allow one
        // to inquiry if any given document of the collection belongs indeed to this shard or if it
        // is coming from (or a vestige of) an ongoing migration.
        CollectionMetadataPtr _collMetadata;

        //
        // The underlying execution machinery.
        //

        // The new world: a runner.
        scoped_ptr<Runner> _runner;

        //
        // Cursor-only private data and methods.  DEPRECATED.
        //

        // The old world: a cursor.  DEPRECATED.
        const shared_ptr<Cursor> _c;
        
        /**
         * call when cursor's location changes so that we can update the cursorsbylocation map.  if
         * you are locked and internally iterating, only need to call when you are ready to
         * "unlock".
         */
        void updateLocation();
        void setLastLoc_inlock(DiskLoc);
        Record* _recordForYield( RecordNeeds need );

        DiskLoc _lastLoc;                        // use getter and setter not this (important)
        bool _doingDeletes; // when true we are the delete and aboutToDelete shouldn't manipulate us

        // TODO: This will be moved into the runner.
        ElapsedTracker _yieldSometimesTracker;
    };

    /**
     * use this to assure we don't in the background time out cursor while it is under use.  if you
     * are using noTimeout() already, there is no risk anyway.  Further, this mechanism guards
     * against two getMore requests on the same cursor executing at the same time - which might be
     * bad.  That should never happen, but if a client driver had a bug, it could (or perhaps some
     * sort of attack situation).
    */
    class ClientCursorPin : boost::noncopyable {
    public:
        ClientCursorPin( long long cursorid );
        ~ClientCursorPin();
        void release();
        // Call this to delete the underlying ClientCursor.
        void free();
        ClientCursor *c() const;
    private:
        CursorId _cursorid;
    };

    /** Assures safe and reliable cleanup of a ClientCursor. */
    class ClientCursorHolder : boost::noncopyable {
    public:
        ClientCursorHolder( ClientCursor *c = 0 );
        ~ClientCursorHolder();
        void reset( ClientCursor *c = 0 );
        ClientCursor* get();
        operator bool() { return _c; }
        ClientCursor * operator-> ();
        const ClientCursor * operator-> () const;
        /** Release ownership of the ClientCursor. */
        void release();
    private:
        ClientCursor *_c;
        CursorId _id;
    };

    /** thread for timing out old cursors */
    class ClientCursorMonitor : public BackgroundJob {
    public:
        string name() const { return "ClientCursorMonitor"; }
        void run();
    };
        
    struct ClientCursorYieldLock : boost::noncopyable {
        explicit ClientCursorYieldLock( ptr<ClientCursor> cc );
        ~ClientCursorYieldLock();

        /**
         * @return if the cursor is still ok
         *         if it is, we also relock
         */
        bool stillOk();
        void relock();

        private:
        const bool _canYield;
        ClientCursor::YieldData _data;
        scoped_ptr<dbtempreleasecond> _unlock;
    };

} // namespace mongo

// ClientCursor should only be used with auto_ptr because it needs to be
// release()ed after a yield if stillOk() returns false and these pointer types
// do not support releasing. This will prevent them from being used accidentally
// Instead of auto_ptr<>, which still requires some degree of manual management
// of this, consider using ClientCursor::Holder which handles ClientCursor's
// unusual self-deletion mechanics.
namespace boost{
    template<> class scoped_ptr<mongo::ClientCursor> {};
    template<> class shared_ptr<mongo::ClientCursor> {};
}
