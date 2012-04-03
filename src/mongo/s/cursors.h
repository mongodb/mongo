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
#include "../client/parallel.h"

#include "request.h"

namespace mongo {

    class ShardedClientCursor : boost::noncopyable {
    public:
        ShardedClientCursor( QueryMessage& q , ClusteredCursor * cursor );
        virtual ~ShardedClientCursor();

        long long getId();

        /**
         * @return the cumulative number of documents seen by this cursor.
         */
        int getTotalSent() const;

        /**
         * Sends queries to the shards, gather the result for this batch and sends the response
         * to the socket.
         *
         * @return whether there is more data left
         */
        bool sendNextBatchAndReply( Request& r );

        /**
         * Sends queries to the shards and gather the result for this batch.
         *
         * @param r The request object from the client
         * @param ntoreturn Number of documents to return
         * @param buffer The buffer to use to store the results.
         * @param docCount This will contain the number of documents gathered for this batch after
         *        a successful call.
         *
         * @return true if this is not the final batch.
         */
        bool sendNextBatch( Request& r, int ntoreturn, BufBuilder& buffer, int& docCount );

        void accessed();
        /** @return idle time in ms */
        long long idleTime( long long now );

        // The default initial buffer size for sending responses.
        static const int INIT_REPLY_BUFFER_SIZE;

    protected:

        ClusteredCursor * _cursor;

        int _skip;
        int _ntoreturn;

        int _totalSent;
        bool _done;

        long long _id;
        long long _lastAccessMillis; // 0 means no timeout

    };

    typedef boost::shared_ptr<ShardedClientCursor> ShardedClientCursorPtr;

    class CursorCache {
    public:

        static long long TIMEOUT;

        typedef map<long long,ShardedClientCursorPtr> MapSharded;
        typedef map<long long,string> MapNormal;

        CursorCache();
        ~CursorCache();

        ShardedClientCursorPtr get( long long id ) const;
        void store( ShardedClientCursorPtr cursor );
        void remove( long long id );

        void storeRef( const string& server , long long id );

        /** @return the server for id or "" */
        string getRef( long long id ) const ;
        
        void gotKillCursors(Message& m );

        void appendInfo( BSONObjBuilder& result ) const ;

        long long genId();

        void doTimeouts();
        void startTimeoutThread();
    private:
        mutable mongo::mutex _mutex;

        MapSharded _cursors;
        MapNormal _refs;

        long long _shardedTotal;

        static const int _myLogLevel;
    };

    extern CursorCache cursorCache;
}
