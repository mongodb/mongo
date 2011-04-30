// @file writeback_listener.h

/**
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

#include "../client/connpool.h"
#include "../util/background.h"
#include "../db/client.h"

namespace mongo {

    /*
     * The writeback listener takes back write attempts that were made against a wrong shard.
     * (Wrong here in the sense that the target chunk moved before this mongos had a chance to
     * learn so.) It is responsible for reapplying these writes to the correct shard.
     *
     * Runs (instantiated) on mongos.
     * Currently, there is one writebacklistener per shard.
     */
    class WriteBackListener : public BackgroundJob {
    public:
        static void init( DBClientBase& conn );
        static void init( const string& host );

        static BSONObj waitFor( ConnectionId connectionId, const OID& oid );

    protected:
        WriteBackListener( const string& addr );

        string name() const { return "WriteBackListener"; }
        void run();

    private:
        string _addr;
        
        static mongo::mutex _cacheLock; // protects _cache
        static map<string,WriteBackListener*> _cache; // server to listener
        static set<string> _seenSets; // cache of set urls we've seen - note this is ever expanding for order, case, changes

        struct WBStatus {
            OID id;
            BSONObj gle;
        };

        static mongo::mutex _seenWritebacksLock;  // protects _seenWritbacks
        static map<ConnectionId,WBStatus> _seenWritebacks; // connectionId -> last write back GLE
    };

    void waitForWriteback( const OID& oid );

}  // namespace mongo
