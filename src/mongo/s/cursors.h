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


#pragma once

#include "mongo/pch.h"

#include <string>

#include "mongo/client/parallel.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/s/request.h"

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

        std::string getNS() { return _cursor->getNS(); }

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

        void storeRef(const std::string& server, long long id, const std::string& ns);
        void removeRef( long long id );

        /** @return the server for id or "" */
        string getRef( long long id ) const ;
        /** @return the ns for id or "" */
        std::string getRefNS(long long id) const ;
        
        void gotKillCursors(Message& m );

        void appendInfo( BSONObjBuilder& result ) const ;

        long long genId();

        void doTimeouts();
        void startTimeoutThread();
    private:
        mutable mongo::mutex _mutex;

        PseudoRandom _random;

        MapSharded _cursors;
        MapNormal _refs; // Maps cursor ID to shard name
        MapNormal _refsNS; // Maps cursor ID to namespace
        
        long long _shardedTotal;

        static const int _myLogLevel;
    };

    extern CursorCache cursorCache;
}
