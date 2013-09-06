// @file d_writeback.h

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

#include "mongo/db/jsobj.h"
#include "mongo/util/queue.h"
#include "mongo/util/background.h"

namespace mongo {

    /*
     * The WriteBackManager keeps one queue of pending operations per mongos. The operations get here
     * if they were directed to a chunk that is no longer in this mongod server. The operations are
     * "written back" to the mongos server per its request (command 'writebacklisten').
     *
     * The class is thread safe.
     */
    class WriteBackManager {
    public:

        class QueueInfo : boost::noncopyable {
        public:
            QueueInfo(){}

            BlockingQueue<BSONObj> queue;
            long long lastCall;   // this is elapsed millis since startup
        };

        // a map from mongos's serverIDs to queues of "rejected" operations
        // an operation is rejected if it targets data that does not live on this shard anymore
        typedef map<string,shared_ptr<QueueInfo> > WriteBackQueuesMap;


    public:
        WriteBackManager();
        ~WriteBackManager();

        /*
         * @param remote server ID this operation came from
         * @param op the operation itself
         *
         * Enqueues operation 'op' in server 'remote's queue. The operation will be written back to
         * remote at a later stage.
         *
         * @return the writebackId generated
         */
        OID queueWriteBack( const string& remote , BSONObjBuilder& opBuilder );

        /*
         * @param remote server ID
         * @return the queue for operations that came from 'remote'
         *
         * Gets access to server 'remote's queue, which is synchronized.
         */
        shared_ptr<QueueInfo> getWritebackQueue( const string& remote );

        /*
         * @return true if there is no operation queued for write back
         */
        bool queuesEmpty() const;

        /** 
         * appends a number of statistics
         */
        void appendStats( BSONObjBuilder& b ) const;
        
        /**
         * removes queues that have been idle
         * @return if something was removed
         */
        bool cleanupOldQueues();
        
    private:
        
        // '_writebackQueueLock' protects only the map itself, since each queue is synchronized.
        mutable mongo::mutex _writebackQueueLock;
        WriteBackQueuesMap _writebackQueues;
        
        class Cleaner : public PeriodicTask {
        public:
            virtual string taskName() const { return "WriteBackManager::cleaner"; }
            virtual void taskDoWork();
        };

        Cleaner _cleaner;
    };

    // TODO collect global state in a central place and init during startup
    extern WriteBackManager writeBackManager;

} // namespace mongo
