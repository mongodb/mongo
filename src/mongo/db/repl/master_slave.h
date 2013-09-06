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

#include "mongo/db/repl/oplogreader.h"

/* replication data overview

   at the slave:
     local.sources { host: ..., source: ..., only: ..., syncedTo: ..., localLogTs: ..., dbsNextPass: { ... }, incompleteCloneDbs: { ... } }

   at the master:
     local.oplog.$<source>
*/

namespace mongo {

    // Main entry point for master/slave at startup time.
    void startMasterSlave();

    // Global variable that contains a string telling why master/slave halted
    extern const char *replAllDead;

    /* A replication exception */
    class SyncException : public DBException {
    public:
        SyncException() : DBException( "sync exception" , 10001 ) {}
    };

    namespace threadpool {
        class ThreadPool;
    }

    /* A Source is a source from which we can pull (replicate) data.
       stored in collection local.sources.

       Can be a group of things to replicate for several databases.

          { host: ..., source: ..., only: ..., syncedTo: ..., dbsNextPass: { ... }, incompleteCloneDbs: { ... } }

       'source' defaults to 'main'; support for multiple source names is
       not done (always use main for now).
    */
    class ReplSource {
        shared_ptr<threadpool::ThreadPool> tp;

        void resync(const std::string& dbName);

        /** @param alreadyLocked caller already put us in write lock if true */
        void sync_pullOpLog_applyOperation(BSONObj& op, bool alreadyLocked);

        /* pull some operations from the master's oplog, and apply them.
           calls sync_pullOpLog_applyOperation
        */
        int sync_pullOpLog(int& nApplied);

        /* we only clone one database per pass, even if a lot need done.  This helps us
           avoid overflowing the master's transaction log by doing too much work before going
           back to read more transactions. (Imagine a scenario of slave startup where we try to
           clone 100 databases in one pass.)
        */
        set<string> addDbNextPass;

        set<string> incompleteCloneDbs;

        BSONObj _me;

        ReplSource();

        // returns the dummy ns used to do the drop
        string resyncDrop( const char *db, const char *requester );
        // call without the db mutex
        void syncToTailOfRemoteLog();
        string ns() const { return string( "local.oplog.$" ) + sourceName(); }
        unsigned _sleepAdviceTime;

        /**
         * If 'db' is a new database and its name would conflict with that of
         * an existing database, synchronize these database names with the
         * master.
         * @return true iff an op with the specified ns may be applied.
         */
        bool handleDuplicateDbName( const BSONObj &op, const char *ns, const char *db );

        // populates _me so that it can be passed to oplogreader for handshakes
        void ensureMe();

    public:
        OplogReader oplogReader;

        void applyOperation(const BSONObj& op);
        string hostName;    // ip addr or hostname plus optionally, ":<port>"
        string _sourceName;  // a logical source name.
        string sourceName() const { return _sourceName.empty() ? "main" : _sourceName; }
        string only; // only a certain db. note that in the sources collection, this may not be changed once you start replicating.

        /* the last time point we have already synced up to (in the remote/master's oplog). */
        OpTime syncedTo;

        int nClonedThisPass;

        typedef vector< shared_ptr< ReplSource > > SourceVector;
        static void loadAll(SourceVector&);
        explicit ReplSource(BSONObj);

        /* -1 = error */
        int sync(int& nApplied);

        void save(); // write ourself to local.sources

        // make a jsobj from our member fields of the form
        //   { host: ..., source: ..., syncedTo: ... }
        BSONObj jsobj();

        bool operator==(const ReplSource&r) const {
            return hostName == r.hostName && sourceName() == r.sourceName();
        }
        string toString() const { return sourceName() + "@" + hostName; }

        bool haveMoreDbsToSync() const { return !addDbNextPass.empty(); }
        int sleepAdvice() const {
            if ( !_sleepAdviceTime )
                return 0;
            int wait = _sleepAdviceTime - unsigned( time( 0 ) );
            return wait > 0 ? wait : 0;
        }

        static bool throttledForceResyncDead( const char *requester );
        static void forceResyncDead( const char *requester );
        void forceResync( const char *requester );
    };

    /**
     * Helper class used to set and query an ignore state for a named database.
     * The ignore state will expire after a specified OpTime.
     */
    class DatabaseIgnorer {
    public:
        /** Indicate that operations for 'db' should be ignored until after 'futureOplogTime' */
        void doIgnoreUntilAfter( const string &db, const OpTime &futureOplogTime );
        /**
         * Query ignore state of 'db'; if 'currentOplogTime' is after the ignore
         * limit, the ignore state will be cleared.
         */
        bool ignoreAt( const string &db, const OpTime &currentOplogTime );
    private:
        map< string, OpTime > _ignores;
    };

}
