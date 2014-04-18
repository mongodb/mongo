// parallel.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
   tools for working in parallel/sharded/clustered environment
 */

#pragma once

#include "mongo/client/export_macros.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/shard.h"
#include "mongo/s/stale_exception.h"  // for StaleConfigException
#include "mongo/util/concurrency/mvar.h"

namespace mongo {

    /**
     * holder for a server address and a query to run
     */
    class MONGO_CLIENT_API ServerAndQuery {
    public:
        ServerAndQuery( const string& server , BSONObj extra = BSONObj() , BSONObj orderObject = BSONObj() ) :
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

        string toString() const {
            StringBuilder ss;
            ss << "server:" << _server << " _extra:" << _extra.toString() << " _orderObject:" << _orderObject.toString();
            return ss.str();
        }

        operator string() const {
            return toString();
        }

        string _server;
        BSONObj _extra;
        BSONObj _orderObject;
    };

    class ParallelConnectionMetadata;
    class FilteringClientCursor;

    class MONGO_CLIENT_API CommandInfo {
    public:
        string versionedNS;
        BSONObj cmdFilter;

        CommandInfo() {}
        CommandInfo( const string& vns, const BSONObj& filter ) : versionedNS( vns ), cmdFilter( filter ) {}

        bool isEmpty(){
            return versionedNS.size() == 0;
        }

        string toString() const {
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

        ShardConnectionPtr conn;
        DBClientCursorPtr cursor;

        // Version information
        ChunkManagerPtr manager;
        ShardPtr primary;

        // Cursor status information
        long long count;
        bool done;

        BSONObj toBSON() const;

        string toString() const {
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

        string toString() const {
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
        ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , const string& ns ,
                                     const Query& q , int options=0, const BSONObj& fields=BSONObj() );

        ~ParallelSortClusteredCursor();

        std::string getNS();

        /** call before using */
        void init();

        bool more();
        BSONObj next();
        string type() const { return "ParallelSort"; }

        void fullInit();
        void startInit();
        void finishInit();

        bool isCommand(){ return NamespaceString( _qSpec.ns() ).isCommand(); }
        bool isExplain(){ return _qSpec.isExplain(); }
        bool isVersioned(){ return _qShards.size() == 0; }

        bool isSharded();
        ShardPtr getPrimary();
        void getQueryShards( set<Shard>& shards );
        ChunkManagerPtr getChunkManager( const Shard& shard );
        DBClientCursorPtr getShardCursor( const Shard& shard );

        BSONObj toBSON() const;
        string toString() const;

        void explain(BSONObjBuilder& b);

    private:
        void _finishCons();

        void _explain( map< string,list<BSONObj> >& out );

        void _markStaleNS( const NamespaceString& staleNS, const StaleConfigException& e, bool& forceReload, bool& fullReload );
        void _handleStaleNS( const NamespaceString& staleNS, bool forceReload, bool fullReload );

        bool _didInit;
        bool _done;

        set<Shard> _qShards;
        QuerySpec _qSpec;
        CommandInfo _cInfo;

        // Count round-trips req'd for namespaces and total
        map<string,int> _staleNSMap;
        int _totalTries;

        map<Shard,PCMData> _cursorMap;

        // LEGACY BELOW
        int _numServers;
        int _lastFrom;
        set<ServerAndQuery> _servers;
        BSONObj _sortKey;

        FilteringClientCursor * _cursors;
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
        string _ns;
        BSONObj _query;
        int _options;
        BSONObj _fields;
        int _batchSize;
    };


    // TODO:  We probably don't really need this as a separate class.
    class MONGO_CLIENT_API FilteringClientCursor {
    public:
        FilteringClientCursor( const BSONObj filter = BSONObj() );
        FilteringClientCursor( DBClientCursor* cursor , const BSONObj filter = BSONObj() );
        FilteringClientCursor( auto_ptr<DBClientCursor> cursor , const BSONObj filter = BSONObj() );
        ~FilteringClientCursor();

        void reset( auto_ptr<DBClientCursor> cursor );
        void reset( DBClientCursor* cursor, ParallelConnectionMetadata* _pcmData = NULL );

        bool more();
        BSONObj next();

        BSONObj peek();

        DBClientCursor* raw() { return _cursor.get(); }
        ParallelConnectionMetadata* rawMData(){ return _pcmData; }

        // Required for new PCursor
        void release(){
            _cursor.release();
            _pcmData = NULL;
        }

    private:
        void _advance();

        Matcher _matcher;
        auto_ptr<DBClientCursor> _cursor;
        ParallelConnectionMetadata* _pcmData;

        BSONObj _next;
        bool _done;
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

            string getServer() const { return _server; }

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

            CommandResult( const string& server,
                           const string& db,
                           const BSONObj& cmd,
                           int options,
                           DBClientBase * conn,
                           bool useShardedConn );
            void init();

            string _server;
            string _db;
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
        static shared_ptr<CommandResult> spawnCommand( const string& server,
                                                       const string& db,
                                                       const BSONObj& cmd,
                                                       int options,
                                                       DBClientBase * conn = 0,
                                                       bool useShardConn = false );
    };


}

