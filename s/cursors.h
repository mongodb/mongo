// cursors.h
/*
 *    Copyright (C) 2010 10gen Inc.
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
 */


#pragma once 

#include "../pch.h"

#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../client/dbclient.h"
#include "../client/parallel.h"

#include "request.h"

namespace mongo {

    class ShardedClientCursor : boost::noncopyable {
    public:
        ShardedClientCursor( QueryMessage& q , ClusteredCursor * cursor );
        virtual ~ShardedClientCursor();

        long long getId();
        
        /**
         * @return whether there is more data left
         */
        bool sendNextBatch( Request& r ){ return sendNextBatch( r , _ntoreturn ); }
        bool sendNextBatch( Request& r , int ntoreturn );
        
    protected:
        
        ClusteredCursor * _cursor;
        
        int _skip;
        int _ntoreturn;

        int _totalSent;
        bool _done;

        long long _id;
    };

    typedef boost::shared_ptr<ShardedClientCursor> ShardedClientCursorPtr;
    
    class CursorCache {
    public:
        
        typedef map<long long,ShardedClientCursorPtr> MapSharded;
        typedef map<long long,string> MapNormal;

        CursorCache();
        ~CursorCache();
        
        ShardedClientCursorPtr get( long long id );
        void store( ShardedClientCursorPtr cursor );
        void remove( long long id );

        void storeRef( const string& server , long long id );

        void gotKillCursors(Message& m );
        
        void appendInfo( BSONObjBuilder& result );

        long long genId();
    private:
        mutex _mutex;

        MapSharded _cursors;
        MapNormal _refs;
        
        long long _shardedTotal;
    };
    
    extern CursorCache cursorCache;
}
