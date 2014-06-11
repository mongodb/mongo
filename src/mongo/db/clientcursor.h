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

#include <boost/thread/recursive_mutex.hpp>

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/runner.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/util/background.h"
#include "mongo/util/net/message.h"

namespace mongo {

    typedef boost::recursive_mutex::scoped_lock recursive_scoped_lock;
    class ClientCursor;
    class Collection;
    class CurOp;
    class Database;
    class NamespaceDetails;
    class ParsedQuery;

    typedef long long CursorId; /* passed to the client so it can send back on getMore */
    static const CursorId INVALID_CURSOR_ID = -1; // But see SERVER-5726.

    /**
     * ClientCursor is a wrapper that represents a cursorid from our database application's
     * perspective.
     */
    class ClientCursor : private boost::noncopyable {
    public:
        ClientCursor(const Collection* collection, Runner* runner,
                     int qopts = 0, const BSONObj query = BSONObj());

        ClientCursor(const Collection* collection);

        ~ClientCursor();

        //
        // Basic accessors
        //

        CursorId cursorid() const { return _cursorid; }
        std::string ns() const { return _ns; }
        const Collection* collection() const { return _collection; }

        /**
         * This is called when someone is dropping a collection or something else that
         * goes through killing cursors.
         * It removes the responsiilibty of de-registering from ClientCursor.
         * Responsibility for deleting the ClientCursor doesn't change from this call
         * see Runner::kill.
         */
        void kill();

        //
        // Timing and timeouts
        //

        /**
         * @param millis amount of idle passed time since last call
         * note called outside of locks (other than ccmutex) so care must be exercised
         */
        bool shouldTimeout( int millis );
        void setIdleTime( int millis );
        int idleTime() const { return _idleAgeMillis; }

        uint64_t getLeftoverMaxTimeMicros() const { return _leftoverMaxTimeMicros; }
        void setLeftoverMaxTimeMicros( uint64_t leftoverMaxTimeMicros ) {
            _leftoverMaxTimeMicros = leftoverMaxTimeMicros;
        }

        //
        // Sharding-specific data.  TODO: Document.
        //

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

        /**
         * Is this ClientCursor backed by an aggregation pipeline. Defaults to false.
         *
         * Agg Runners differ from others in that they manage their own locking internally and
         * should not be killed or destroyed when the underlying collection is deleted.
         *
         * Note: This should *not* be set for the internal cursor used as input to an aggregation.
         */
        bool isAggCursor;

        unsigned pinValue() const { return _pinValue; }

        static long long totalOpen();

    private:
        friend class ClientCursorMonitor;
        friend class CmdCursorInfo;
        friend class CollectionCursorCache;

        /**
         * Initialization common between both constructors for the ClientCursor. The database must
         * be stable when this is called, because cursors hang off the collection.
         */
        void init();

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
        std::string _ns;

        const Collection* _collection;

        // if we've added it to the total open counter yet
        bool _countedYet;

        // How many objects have been returned by the find() so far?
        int _pos;

        // The query that prompted this ClientCursor.  Only used for debugging.
        BSONObj _query;

        // See the QueryOptions enum in dbclient.h
        int _queryOptions;

        // TODO: document better.
        OpTime _slaveReadTill;

        // How long has the cursor been idle?
        int _idleAgeMillis;

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
        scoped_ptr<Runner> _runner;
    };

    /**
     * use this to assure we don't in the background time out cursor while it is under use.  if you
     * are using noTimeout() already, there is no risk anyway.  Further, this mechanism guards
     * against two getMore requests on the same cursor executing at the same time - which might be
     * bad.  That should never happen, but if a client driver had a bug, it could (or perhaps some
     * sort of attack situation).
     * Must have a read lock on the collection already
    */
    class ClientCursorPin : boost::noncopyable {
    public:
        ClientCursorPin( const Collection* collection, long long cursorid );
        ~ClientCursorPin();
        // This just releases the pin, does not delete the underlying
        // unless ownership has passed to us after kill
        void release();
        // Call this to delete the underlying ClientCursor.
        void deleteUnderlying();
        ClientCursor *c() const;
    private:
        ClientCursor* _cursor;
    };

    /** thread for timing out old cursors */
    class ClientCursorMonitor : public BackgroundJob {
    public:
        std::string name() const { return "ClientCursorMonitor"; }
        void run();
    };

} // namespace mongo
