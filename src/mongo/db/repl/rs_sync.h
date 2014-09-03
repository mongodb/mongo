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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <deque>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/dur.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/sync.h"
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
        SyncTail(BackgroundSyncInterface *q, MultiSyncApplyFunc func);

        virtual ~SyncTail();
        virtual bool syncApply(const BSONObj &o, bool convertUpdateToUpsert = false);

        /**
         * Runs _applyOplogUntil(stopOpTime)
         */
        virtual void oplogApplication(const OpTime& stopOpTime);

        void oplogApplication();
        bool peek(BSONObj* obj);

        class OpQueue {
        public:
            OpQueue() : _size(0) {}
            size_t getSize() { return _size; }
            std::deque<BSONObj>& getDeque() { return _deque; }
            void push_back(BSONObj& op) {
                _deque.push_back(op);
                _size += op.objsize();
            }
            bool empty() {
                return _deque.empty();
            }
            BSONObj back(){
                verify(!_deque.empty());
                return _deque.back();
            }

        private:
            std::deque<BSONObj> _deque;
            size_t _size;
        };

        // returns true if we should continue waiting for BSONObjs, false if we should
        // stop waiting and apply the queue we have.  Only returns false if !ops.empty().
        bool tryPopAndWaitForMore(OpQueue* ops);
        
        // After ops have been written to db, call this
        // to update local oplog.rs, as well as notify the primary
        // that we have applied the ops.
        // Ops are removed from the deque.
        void applyOpsToOplog(std::deque<BSONObj>* ops);

    protected:
        // Cap the batches using the limit on journal commits.
        // This works out to be 100 MB (64 bit) or 50 MB (32 bit)
        static const unsigned int replBatchLimitBytes = dur::UncommittedBytesLimit;
        static const int replBatchLimitSeconds = 1;
        static const unsigned int replBatchLimitOperations = 5000;

        // Prefetch and write a deque of operations.
        void multiApply(std::deque<BSONObj>& ops);

        /**
         * Applies oplog entries until reaching "endOpTime".
         *
         * Returns the OpTime from the last doc applied
         *
         * NOTE:Will not transition or check states
         */
        void _applyOplogUntil(const OpTime& endOpTime);


        // The version of the last op to be read
        int oplogVersion;

    private:
        BackgroundSyncInterface* _networkQueue;

        // Function to use during applyOps
        MultiSyncApplyFunc _applyFunc;

        // Doles out all the work to the reader pool threads and waits for them to complete
        void prefetchOps(const std::deque<BSONObj>& ops);
        // Used by the thread pool readers to prefetch an op
        static void prefetchOp(const BSONObj& op);

        // Doles out all the work to the writer pool threads and waits for them to complete
        void applyOps(const std::vector< std::vector<BSONObj> >& writerVectors);

        void fillWriterVectors(const std::deque<BSONObj>& ops, 
                               std::vector< std::vector<BSONObj> >* writerVectors);
        void handleSlaveDelay(const BSONObj& op);
        void setOplogVersion(const BSONObj& op);
    };

    /**
     * Initial clone and sync
     */
    class InitialSync : public SyncTail {
    public:
        virtual ~InitialSync();
        InitialSync(BackgroundSyncInterface *q);
        virtual void oplogApplication(const OpTime& stopOpTime);

    };

    // TODO: move hbmsg into an error-keeping class (SERVER-4444)
    void sethbmsg(const string& s, const int logLevel=0);

    // These free functions are used by the thread pool workers to write ops to the db.
    void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);
    void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);

} // namespace replset
} // namespace mongo
