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
   tools for wokring in parallel/sharded/clustered environment
 */

#include "../stdafx.h"
#include "dbclient.h"
#include "../db/dbmessage.h"

namespace mongo {

    /**
     * this is a cursor that works over a set of servers
     * can be used in serial/paralellel as controlled by sub classes
     */
    class ClusteredCursor {
    public:
        ClusteredCursor( QueryMessage& q );
        ClusteredCursor( const string& ns , const BSONObj& q , int options=0 , const BSONObj& fields=BSONObj() );
        virtual ~ClusteredCursor();

        virtual bool more() = 0;
        virtual BSONObj next() = 0;
        
        static BSONObj concatQuery( const BSONObj& query , const BSONObj& extraFilter );
        
    protected:
        auto_ptr<DBClientCursor> query( const string& server , int num = 0 , BSONObj extraFilter = BSONObj() );

        static BSONObj _concatFilter( const BSONObj& filter , const BSONObj& extraFilter );
        
        string _ns;
        BSONObj _query;
        int _options;
        BSONObj _fields;

        bool _done;
    };


    /**
     * holder for a server address and a query to run
     */
    class ServerAndQuery {
    public:
        ServerAndQuery( const string& server , BSONObj extra = BSONObj() , BSONObj orderObject = BSONObj() ) : 
            _server( server ) , _extra( extra.getOwned() ) , _orderObject( orderObject.getOwned() ){
        }

        bool operator<( const ServerAndQuery& other ) const{
            if ( ! _orderObject.isEmpty() )
                return _orderObject.woCompare( other._orderObject ) < 0;
            
            if ( _server < other._server )
                return true;
            if ( other._server > _server )
                return false;
            return _extra.woCompare( other._extra ) < 0;
        }

        string _server;
        BSONObj _extra;
        BSONObj _orderObject;
    };


    /**
     * runs a query in serial across any number of servers
     * returns all results from 1 server, then the next, etc...
     */
    class SerialServerClusteredCursor : public ClusteredCursor {
    public:
        SerialServerClusteredCursor( set<ServerAndQuery> servers , QueryMessage& q , int sortOrder=0);
        virtual bool more();
        virtual BSONObj next();
    private:
        vector<ServerAndQuery> _servers;
        unsigned _serverIndex;
        
        auto_ptr<DBClientCursor> _current;
    };


    /**
     * runs a query in parellel across N servers
     * sots
     */        
    class ParallelSortClusteredCursor : public ClusteredCursor {
    public:
        ParallelSortClusteredCursor( set<ServerAndQuery> servers , QueryMessage& q , const BSONObj& sortKey );
        ParallelSortClusteredCursor( set<ServerAndQuery> servers , const string& ns , 
                                     const Query& q , int options=0, const BSONObj& fields=BSONObj() );
        virtual ~ParallelSortClusteredCursor();
        virtual bool more();
        virtual BSONObj next();
    private:
        void _init();
        
        void advance();

        int _numServers;
        set<ServerAndQuery> _servers;
        BSONObj _sortKey;

        auto_ptr<DBClientCursor> * _cursors;
        BSONObj * _nexts;
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
                assert( _done );
                return _ok;
            }

            BSONObj result() const {
                assert( _done );
                return _res;
            }

            /**
               blocks until command is done
               returns ok()
             */
            bool join();
            
        private:
            
            CommandResult( const string& server , const string& db , const BSONObj& cmd );
            
            string _server;
            string _db;
            BSONObj _cmd;

            boost::thread _thr;
            
            BSONObj _res;
            bool _done;
            bool _ok;
            
            friend class Future;
        };
        
        static void commandThread();
        
        static shared_ptr<CommandResult> spawnCommand( const string& server , const string& db , const BSONObj& cmd );

    private:
        static shared_ptr<CommandResult> * _grab;
    };

    
}
