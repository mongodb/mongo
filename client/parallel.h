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

#include "stdafx.h"
#include "dbclient.h"
#include "../db/dbmessage.h"

namespace mongo {

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
        
    class ParallelSortClusteredCursor : public ClusteredCursor {
    public:
        ParallelSortClusteredCursor( set<ServerAndQuery> servers , QueryMessage& q , const BSONObj& sortKey );
        virtual ~ParallelSortClusteredCursor();
        virtual bool more();
        virtual BSONObj next();
    private:
        void advance();

        int _numServers;
        set<ServerAndQuery> _servers;
        BSONObj _sortKey;

        auto_ptr<DBClientCursor> * _cursors;
        BSONObj * _nexts;
    };

}
