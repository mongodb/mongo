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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/client/parallel.h"
#include "mongo/platform/random.h"

namespace mongo {

    class QueryMessage;


    class ShardedClientCursor {
        MONGO_DISALLOW_COPYING(ShardedClientCursor);
    public:
        ShardedClientCursor( QueryMessage& q , ParallelSortClusteredCursor * cursor );
        ~ShardedClientCursor();

        long long getId();

        /**
         * @return the cumulative number of documents seen by this cursor.
         */
        int getTotalSent() const;

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
        bool sendNextBatch(int ntoreturn, BufBuilder& buffer, int& docCount);

        void accessed();
        /** @return idle time in ms */
        long long idleTime( long long now );

        std::string getNS() { return _cursor->getNS(); }

        // The default initial buffer size for sending responses.
        static const int INIT_REPLY_BUFFER_SIZE;

    protected:

        ParallelSortClusteredCursor * _cursor;

        int _skip;
        int _ntoreturn;

        int _totalSent;
        bool _done;

        long long _id;
        long long _lastAccessMillis; // 0 means no timeout

    };

    typedef std::shared_ptr<ShardedClientCursor> ShardedClientCursorPtr;

    class CursorCache {
    public:

        static long long TIMEOUT;

        typedef std::map<long long,ShardedClientCursorPtr> MapSharded;
        typedef std::map<long long,int> MapShardedInt;
        typedef std::map<long long,std::string> MapNormal;

        CursorCache();
        ~CursorCache();

        ShardedClientCursorPtr get( long long id ) const;
        int getMaxTimeMS( long long id ) const;
        void store( ShardedClientCursorPtr cursor, int maxTimeMS );
        void updateMaxTimeMS( long long id, int maxTimeMS );
        void remove( long long id );

        void storeRef(const std::string& server, long long id, const std::string& ns);
        void removeRef( long long id );

        /** @return the server for id or "" */
        std::string getRef( long long id ) const ;
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

        // Maps sharded cursor ID to ShardedClientCursorPtr.
        MapSharded _cursors;

        // Maps sharded cursor ID to remaining max time.  Value can be any of:
        // - the constant "kMaxTimeCursorNoTimeLimit", or
        // - the constant "kMaxTimeCursorTimeLimitExpired", or
        // - a positive integer representing milliseconds of remaining time
        MapShardedInt _cursorsMaxTimeMS;

        // Maps passthrough cursor ID to shard name.
        MapNormal _refs;

        // Maps passthrough cursor ID to namespace.
        MapNormal _refsNS;
        
        long long _shardedTotal;

        static const int _myLogLevel;
    };

    extern CursorCache cursorCache;
}
