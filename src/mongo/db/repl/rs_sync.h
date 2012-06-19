/**
 *    Copyright (C) 2008 10gen Inc.
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

#include <deque>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/oplog.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace replset {

    class BackgroundSyncInterface;

    /**
     * "Normal" replica set syncing
     */
    class SyncTail : public Sync {
        typedef void (*MultiSyncApplyFunc)(const std::vector<BSONObj>& ops, SyncTail* st);
    public:
        SyncTail(BackgroundSyncInterface *q);
        virtual ~SyncTail();
        virtual bool syncApply(const BSONObj &o);
        void oplogApplication();
        bool peek(BSONObj* obj);

        // returns true if we should continue waiting for BSONObjs, false if we should
        // stop waiting and apply the queue we have.  Only returns false if !ops.empty().
        bool tryPopAndWaitForMore(std::deque<BSONObj>* ops);
        
        // After ops have been written to db, call this
        // to update local oplog.rs, as well as notify the primary
        // that we have applied the ops.
        // Ops are removed from the deque.
        void applyOpsToOplog(std::deque<BSONObj>* ops);

    protected:
        static const unsigned int replBatchSize = 128;

        // Prefetch and write a deque of operations, using the supplied function.
        // Initial Sync and Sync Tail each use a different function.
        void multiApply(std::deque<BSONObj>& ops, MultiSyncApplyFunc applyFunc);

    private:
        BackgroundSyncInterface* _networkQueue;

        // Doles out all the work to the reader pool threads and waits for them to complete
        void prefetchOps(const std::deque<BSONObj>& ops);
        // Used by the thread pool readers to prefetch an op
        static void prefetchOp(const BSONObj& op);

        // Doles out all the work to the writer pool threads and waits for them to complete
        void applyOps(const std::vector< std::vector<BSONObj> >& writerVectors, 
                      MultiSyncApplyFunc applyFunc);

        void fillWriterVectors(const std::deque<BSONObj>& ops, 
                               std::vector< std::vector<BSONObj> >* writerVectors);
        void handleSlaveDelay(const BSONObj& op);
    };

    /**
     * Initial clone and sync
     */
    class InitialSync : public SyncTail {
    public:
        virtual ~InitialSync();
        InitialSync(BackgroundSyncInterface *q);
        void oplogApplication(const BSONObj& applyGTEObj, const BSONObj& minValidObj);
    };

    // TODO: move hbmsg into an error-keeping class (SERVER-4444)
    void sethbmsg(const string& s, const int logLevel=0);

    // These free functions are used by the thread pool workers to write ops to the db.
    void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);
    void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);

} // namespace replset
} // namespace mongo
