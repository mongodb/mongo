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
    class ServerAndQuery {
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

    /**
     * this is a cursor that works over a set of servers
     * can be used in serial/parallel as controlled by sub classes
     */
    class ClusteredCursor {
    public:
        ClusteredCursor( const QuerySpec& q );
        ClusteredCursor( QueryMessage& q );
        ClusteredCursor( const string& ns , const BSONObj& q , int options=0 , const BSONObj& fields=BSONObj() );
        virtual ~ClusteredCursor();

        /** call before using */
        void init();

        virtual std::string getNS() { return _ns; }

        virtual bool more() = 0;
        virtual BSONObj next() = 0;

        static BSONObj concatQuery( const BSONObj& query , const BSONObj& extraFilter );

        virtual string type() const = 0;

        virtual void explain(BSONObjBuilder& b) = 0;

    protected:

        virtual void _init() = 0;

        auto_ptr<DBClientCursor> query( const string& server , int num = 0 , BSONObj extraFilter = BSONObj() , int skipLeft = 0 , bool lazy=false );
        BSONObj explain( const string& server , BSONObj extraFilter = BSONObj() );

        /**
         * checks the cursor for any errors
         * will throw an exceptionif an error is encountered
         */
        void _checkCursor( DBClientCursor * cursor );

        static BSONObj _concatFilter( const BSONObj& filter , const BSONObj& extraFilter );

        virtual void _explain( map< string,list<BSONObj> >& out ) = 0;

        string _ns;
        BSONObj _query;
        BSONObj _hint;
        BSONObj _sort;

        int _options;
        BSONObj _fields;
        int _batchSize;

        bool _didInit;

        bool _done;
    };

    class ParallelConnectionMetadata;

    // TODO:  We probably don't really need this as a separate class.
    class FilteringClientCursor {
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


    class Servers {
    public:
        Servers() {
        }

        void add( const ServerAndQuery& s ) {
            add( s._server , s._extra );
        }

        void add( const string& server , const BSONObj& filter ) {
            vector<BSONObj>& mine = _filters[server];
            mine.push_back( filter.getOwned() );
        }

        // TOOO: pick a less horrible name
        class View {
            View( const Servers* s ) {
                for ( map<string, vector<BSONObj> >::const_iterator i=s->_filters.begin(); i!=s->_filters.end(); ++i ) {
                    _servers.push_back( i->first );
                    _filters.push_back( i->second );
                }
            }
        public:
            int size() const {
                return _servers.size();
            }

            string getServer( int n ) const {
                return _servers[n];
            }

            vector<BSONObj> getFilter( int n ) const {
                return _filters[ n ];
            }

        private:
            vector<string> _servers;
            vector< vector<BSONObj> > _filters;

            friend class Servers;
        };

        View view() const {
            return View( this );
        }


    private:
        map<string, vector<BSONObj> > _filters;

        friend class View;
    };


    /**
     * runs a query in serial across any number of servers
     * returns all results from 1 server, then the next, etc...
     */
    class SerialServerClusteredCursor : public ClusteredCursor {
    public:
        SerialServerClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , int sortOrder=0);
        virtual bool more();
        virtual BSONObj next();
        virtual string type() const { return "SerialServer"; }

    protected:
        virtual void _explain( map< string,list<BSONObj> >& out );

        void _init() {}

        vector<ServerAndQuery> _servers;
        unsigned _serverIndex;

        FilteringClientCursor _current;

        int _needToSkip;
    };



    class CommandInfo {
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

    class ParallelConnectionState {
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

    class ParallelConnectionMetadata {
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
     * Runs a query in parallel across N servers.  New logic has several modes -
     * 1) Standard query, enforces compatible chunk versions for queries across all results
     * 2) Standard query, sent to particular servers with no compatible chunk version enforced, but handling
     *    stale configuration exceptions
     * 3) Command query, either enforcing compatible chunk versions or sent to particular shards.
     */
    class ParallelSortClusteredCursor : public ClusteredCursor {
    public:

        ParallelSortClusteredCursor( const QuerySpec& qSpec, const CommandInfo& cInfo = CommandInfo() );
        ParallelSortClusteredCursor( const set<Shard>& servers, const QuerySpec& qSpec );

        // LEGACY Constructors
        ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , const BSONObj& sortKey );
        ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , const string& ns ,
                                     const Query& q , int options=0, const BSONObj& fields=BSONObj() );

        virtual ~ParallelSortClusteredCursor();
        virtual bool more();
        virtual BSONObj next();
        virtual string type() const { return "ParallelSort"; }

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

        virtual void explain(BSONObjBuilder& b);

    protected:
        void _finishCons();
        void _init();
        void _oldInit();

        virtual void _explain( map< string,list<BSONObj> >& out );

        void _markStaleNS( const NamespaceString& staleNS, const StaleConfigException& e, bool& forceReload, bool& fullReload );
        void _handleStaleNS( const NamespaceString& staleNS, bool forceReload, bool fullReload );

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

    private:
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
    };

    /**
     * tools for doing asynchronous operations
     * right now uses underlying sync network ops and uses another thread
     * should be changed to use non-blocking io
     */
    class Future {
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

            CommandResult( const string& server , const string& db , const BSONObj& cmd , int options , DBClientBase * conn );
            void init();

            string _server;
            string _db;
            int _options;
            BSONObj _cmd;
            DBClientBase * _conn;
            scoped_ptr<ScopedDbConnection> _connHolder; // used if not provided a connection

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
         */
        static shared_ptr<CommandResult> spawnCommand( const string& server , const string& db , const BSONObj& cmd , int options , DBClientBase * conn = 0 );
    };


}

