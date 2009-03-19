// cursors.h

#pragma once 

#include "../stdafx.h"

#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../client/dbclient.h"

#include "request.h"

namespace mongo {

    class ShardedCursor {
    public:
        ShardedCursor( QueryMessage& q );
        virtual ~ShardedCursor();

        virtual bool more() = 0;
        virtual BSONObj next() = 0;

        long long getId(){ return _id; }
        
        /**
         * @return whether there is more data left
         */
        bool sendNextBatch( Request& r ){ return sendNextBatch( r , _ntoreturn ); }
        bool sendNextBatch( Request& r , int ntoreturn );
        
    protected:
        auto_ptr<DBClientCursor> query( const string& server , int num = 0 , BSONObj extraFilter = BSONObj() );

        BSONObj concatQuery( const BSONObj& query , const BSONObj& extraFilter );
        BSONObj _concatFilter( const BSONObj& filter , const BSONObj& extraFilter );

        string _ns;
        int _options;
        int _skip;
        int _ntoreturn;
        
        BSONObj _query;
        BSONObj _fields;

        long long _id;

        int _totalSent;
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

    class SerialServerShardedCursor : public ShardedCursor {
    public:
        SerialServerShardedCursor( set<ServerAndQuery> servers , QueryMessage& q , int sortOrder=0);
        virtual bool more();
        virtual BSONObj next();
    private:
        vector<ServerAndQuery> _servers;
        unsigned _serverIndex;
        
        auto_ptr<DBClientCursor> _current;
    };
        
    class ParallelSortShardedCursor : public ShardedCursor {
    public:
        ParallelSortShardedCursor( set<ServerAndQuery> servers , QueryMessage& q , const BSONObj& sortKey );
        virtual ~ParallelSortShardedCursor();
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
    
    class CursorCache {
    public:
        CursorCache();
        ~CursorCache();
        
        ShardedCursor* get( long long id );
        void store( ShardedCursor* cursor );
        void remove( long long id );

    private:
        map<long long,ShardedCursor*> _cursors;
    };
    
    extern CursorCache cursorCache;
}
