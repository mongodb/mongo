// parallel.h

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
   tools for working in parallel/sharded/clustered environment
 */

#pragma once

#include "mongo/client/export_macros.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/shard.h"
#include "mongo/util/concurrency/mvar.h"

namespace mongo {

    class StaleConfigException;

    /**
     * holder for a server address and a query to run
     */
    class MONGO_CLIENT_API ServerAndQuery {
    public:
        ServerAndQuery( const std::string& server , BSONObj extra = BSONObj() , BSONObj orderObject = BSONObj() ) :
            _server( server ) , _extra( extra.getOwned() ) , _orderObject( orderObject.getOwned() ) {
        }

        bool operator<( const ServerAndQuery& other ) const {
            if ( ! _orderObject.isEmpty() )
                return _orderObject.woCompare( other._orderObject ) < 0;

            if ( _server < other._server )
                return true;
            if ( other._server > _server )
                return false;
            return _extra.woCompare( other._extra ) < 0;
        }

        std::string toString() const {
            StringBuilder ss;
            ss << "server:" << _server << " _extra:" << _extra.toString() << " _orderObject:" << _orderObject.toString();
            return ss.str();
        }

        operator std::string() const {
            return toString();
        }

        std::string _server;
        BSONObj _extra;
        BSONObj _orderObject;
    };

    class ParallelConnectionMetadata;
    class DBClientCursorHolder;

    class MONGO_CLIENT_API CommandInfo {
    public:
        std::string versionedNS;
        BSONObj cmdFilter;

        CommandInfo() {}
        CommandInfo( const std::string& vns, const BSONObj& filter ) : versionedNS( vns ), cmdFilter( filter ) {}

        bool isEmpty(){
            return versionedNS.size() == 0;
        }

        std::string toString() const {
            return str::stream() << "CInfo " << BSON( "v_ns" << versionedNS << "filter" << cmdFilter );
        }
    };

    typedef shared_ptr<ShardConnection> ShardConnectionPtr;

    class DBClientCursor;
    typedef shared_ptr<DBClientCursor> DBClientCursorPtr;

    class MONGO_CLIENT_API ParallelConnectionState {
    public:

        ParallelConnectionState() :
            count( 0 ), done( false ) { }

        // Please do not reorder. cursor destructor can use conn.
        // On a related note, never attempt to cleanup these pointers manually.
        ShardConnectionPtr conn;
        DBClientCursorPtr cursor;

        // Version information
        ChunkManagerPtr manager;
        ShardPtr primary;

        // Cursor status information
        long long count;
        bool done;

        BSONObj toBSON() const;

        std::string toString() const {
            return str::stream() << "PCState : " << toBSON();
        }
    };

    typedef ParallelConnectionState PCState;
    typedef shared_ptr<PCState> PCStatePtr;

    class MONGO_CLIENT_API ParallelConnectionMetadata {
    public:

        ParallelConnectionMetadata() :
            retryNext( false ), initialized( false ), finished( false ), completed( false ), errored( false ) { }

        ~ParallelConnectionMetadata(){
            cleanup( true );
        }

        void cleanup( bool full = true );

        PCStatePtr pcState;

        bool retryNext;

        bool initialized;
        bool finished;
        bool completed;

        bool errored;

        BSONObj toBSON() const;

        std::string toString() const {
            return str::stream() << "PCMData : " << toBSON();
        }
    };

    typedef ParallelConnectionMetadata PCMData;
    typedef shared_ptr<PCMData> PCMDataPtr;

    /**
     * Runs a query in parallel across N servers, enforcing compatible chunk versions for queries
     * across all shards.
     *
     * If CommandInfo is provided, the ParallelCursor does not use the direct .$cmd namespace in the
     * query spec, but instead enforces versions across another namespace specified by CommandInfo.
     * This is to support commands like:
     * db.runCommand({ fileMD5 : "<coll name>" })
     *
     * There is a deprecated legacy mode as well which effectively does a merge-sort across a number
     * of servers, but does not correctly enforce versioning (used only in mapreduce).
     */
    class MONGO_CLIENT_API ParallelSortClusteredCursor {
    public:

        ParallelSortClusteredCursor( const QuerySpec& qSpec, const CommandInfo& cInfo = CommandInfo() );

        // DEPRECATED legacy constructor for pure mergesort functionality - do not use
        ParallelSortClusteredCursor( const std::set<ServerAndQuery>& servers , const std::string& ns ,
                                     const Query& q , int options=0, const BSONObj& fields=BSONObj() );

        ~ParallelSortClusteredCursor();

        std::string getNS();

        /** call before using */
        void init();

        bool more();
        BSONObj next();
        std::string type() const { return "ParallelSort"; }

        void fullInit();
        void startInit();
        void finishInit();

        bool isCommand(){ return NamespaceString( _qSpec.ns() ).isCommand(); }
        bool isExplain(){ return _qSpec.isExplain(); }

        /**
         * Sets the batch size on all underlying cursors to 'newBatchSize'.
         */
        void setBatchSize(int newBatchSize);

        /**
         * Returns whether the collection was sharded when the cursors were established.
         */
        bool isSharded();

        /**
         * Returns the number of shards with open cursors.
         */
        int getNumQueryShards();

        /**
         * Returns the set of shards with open cursors.
         */
        void getQueryShards(std::set<Shard>& shards);

        /**
         * Returns the single shard with an open cursor.
         * It is an error to call this if getNumQueryShards() > 1
         */
        ShardPtr getQueryShard();

        /**
         * Returns primary shard with an open cursor.
         * It is an error to call this if the collection is sharded.
         */
        ShardPtr getPrimary();

        ChunkManagerPtr getChunkManager( const Shard& shard );
        DBClientCursorPtr getShardCursor( const Shard& shard );

        BSONObj toBSON() const;
        std::string toString() const;

        void explain(BSONObjBuilder& b);

    private:
        void _finishCons();

        void _explain( std::map< std::string,std::list<BSONObj> >& out );

        void _markStaleNS( const NamespaceString& staleNS, const StaleConfigException& e, bool& forceReload, bool& fullReload );
        void _handleStaleNS( const NamespaceString& staleNS, bool forceReload, bool fullReload );

        bool _didInit;
        bool _done;

        QuerySpec _qSpec;
        CommandInfo _cInfo;

        // Count round-trips req'd for namespaces and total
        std::map<std::string,int> _staleNSMap;
        int _totalTries;

        std::map<Shard,PCMData> _cursorMap;

        // LEGACY BELOW
        int _numServers;
        int _lastFrom;
        std::set<ServerAndQuery> _servers;
        BSONObj _sortKey;

        DBClientCursorHolder * _cursors;
        int _needToSkip;

        /**
         * Setups the shard version of the connection. When using a replica
         * set connection and the primary cannot be reached, the version
         * will not be set if the slaveOk flag is set.
         */
        void setupVersionAndHandleSlaveOk( PCStatePtr state /* in & out */,
                           const Shard& shard,
                           ShardPtr primary /* in */,
                           const NamespaceString& ns,
                           const std::string& vinfo,
                           ChunkManagerPtr manager /* in */ );

        // LEGACY init - Needed for map reduce
        void _oldInit();

        // LEGACY - Needed ONLY for _oldInit
        std::string _ns;
        BSONObj _query;
        int _options;
        BSONObj _fields;
        int _batchSize;
    };


    /**
     * Helper class to manage ownership of opened cursors while merging results.
     *
     * TODO:  Choose one set of ownership semantics so that this isn't needed - merge sort via
     * mapreduce is the main issue since it has no metadata and this holder owns the cursors.
     */
    class MONGO_CLIENT_API DBClientCursorHolder {
    public:

        DBClientCursorHolder() {}
        ~DBClientCursorHolder() {}

        void reset(DBClientCursor* cursor, ParallelConnectionMetadata* pcmData) {
            _cursor.reset(cursor);
            _pcmData.reset(pcmData);
        }

        DBClientCursor* get() { return _cursor.get(); }
        ParallelConnectionMetadata* getMData() { return _pcmData.get(); }

        void release() {
            _cursor.release();
            _pcmData.release();
        }

    private:

        std::auto_ptr<DBClientCursor> _cursor;
        std::auto_ptr<ParallelConnectionMetadata> _pcmData;
    };

    /**
     * Generally clients should be using Strategy::commandOp() wherever possible - the Future API
     * does not handle versioning.
     *
     * tools for doing asynchronous operations
     * right now uses underlying sync network ops and uses another thread
     * should be changed to use non-blocking io
     */
    class MONGO_CLIENT_API Future {
    public:
        class CommandResult {
        public:

            std::string getServer() const { return _server; }

            bool isDone() const { return _done; }

            bool ok() const {
                verify( _done );
                return _ok;
            }

            BSONObj result() const {
                verify( _done );
                return _res;
            }

            /**
               blocks until command is done
               returns ok()
             */
            bool join( int maxRetries = 1 );

        private:

            CommandResult( const std::string& server,
                           const std::string& db,
                           const BSONObj& cmd,
                           int options,
                           DBClientBase * conn,
                           bool useShardedConn );
            void init();

            std::string _server;
            std::string _db;
            int _options;
            BSONObj _cmd;
            DBClientBase * _conn;
            scoped_ptr<AScopedConnection> _connHolder; // used if not provided a connection
            bool _useShardConn;

            scoped_ptr<DBClientCursor> _cursor;

            BSONObj _res;
            bool _ok;
            bool _done;

            friend class Future;
        };


        /**
         * @param server server name
         * @param db db name
         * @param cmd cmd to exec
         * @param conn optional connection to use.  will use standard pooled if non-specified
         * @param useShardConn use ShardConnection
         */
        static shared_ptr<CommandResult> spawnCommand( const std::string& server,
                                                       const std::string& db,
                                                       const BSONObj& cmd,
                                                       int options,
                                                       DBClientBase * conn = 0,
                                                       bool useShardConn = false );
    };


}

