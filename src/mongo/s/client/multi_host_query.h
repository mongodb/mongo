/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/thread.hpp>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

    //
    // NOTE TO DEVS
    // This is probably not what we want long-term - think very carefully before letting any of the
    // functionality below escape this file.
    //

    class HostThreadPools;
    class HostThreadPool;

    /**
     * A MultiHostQueryOp manages a query operation across multiple hosts.  Supports returning
     * immediately when any host has results or when all hosts have (connectivity) errors.
     *
     * The QueryOp itself dispatches work to the thread pool, and does not wait for all work to be
     * complete before destruction.  This class is not intended to be used by multiple clients at
     * once without external synchronization (for now).
     *
     * Cannot be reused once all query results and errors have been returned.
     */
    class MultiHostQueryOp {
        MONGO_DISALLOW_COPYING(MultiHostQueryOp);
    public:

        /**
         * Network and time services interface
         */
        class SystemEnv;

        /**
         * Constructs a MultiHostQueryOp.  Allows running a query across multiple hosts with a
         * blocking interface.  The lifetime of this class can be shorter than the lifetime of the
         * queries sent via queryAny, freeing up the caller to do further work when any host is fast
         * to respond.
         *
         * The systemEnv and hostThreads must remain in scope while the query op remains in scope.
         *
         * NOTE: SystemEnv* MUST remain valid for as long as hostThreads remains valid, since this
         * operation may schedule background queries but fall out of scope while one of those
         * background queries is still in-progress.
         */
        MultiHostQueryOp(SystemEnv* systemEnv, HostThreadPools* hostThreads);

        ~MultiHostQueryOp();

        /**
         * Blocks for a query to be run on any of the hosts, and returns the fastest result as soon
         * as it becomes available.  This function may only be executed once.
         *
         * If one or more hosts have an error sending/recv'ing the query, the error or composite
         * error is returned if no other hosts are responsive after the timeout period.  Note that
         * this does not apply to errors successfully returned from remote hosts - this is a
         * successful query with an error.
         *
         * Caller owns the returned result if OK.
         */
        StatusWith<DBClientCursor*> queryAny(const std::vector<ConnectionString>& hosts,
                                             const QuerySpec& query,
                                             int timeoutMillis);

        //
        // Below is exposed for testing *only*
        //

        /**
         * Schedules the query work on each of the hosts using the thread pool, with a timeout
         * indicating how long the work is useful for.  Can be called only once.
         */
        void scheduleQuery(const std::vector<ConnectionString>& hosts,
                           const QuerySpec& query,
                           Date_t timeoutAtMillis);

        /**
         * Blocks and waits for the next successful query result or any errors once the timeout is
         * reached.
         * Can be called multiple times until results from all hosts are returned or !OK.
         */
        StatusWith<DBClientCursor*> waitForNextResult(Date_t timeoutAtMillis);

    private:

        /**
         * Data required to execute a query operation by a callback on an arbitrary thread.
         * Information from the dispatching parent op may not be available if the parent is no
         * longer in scope.
         */
        struct PendingQueryContext {

            PendingQueryContext(const ConnectionString& host,
                                const QuerySpec& query,
                                const Date_t timeoutAtMillis,
                                MultiHostQueryOp* parentOp);

            void doBlockingQuery();

            const ConnectionString host;
            const QuerySpec query;
            const Date_t timeoutAtMillis;

            // Must be held to access the parent pointer below
            stdx::mutex parentMutex;
            // Set and unset by the parent operation on scheduling and destruction
            MultiHostQueryOp* parentOp;
        };

        /**
         * Called by a scheduled query (generally on a different thread from the waiting client)
         * when a result is ready from a particular host.
         */
        void noteResult(const ConnectionString& host, StatusWith<DBClientCursor*> result);

        /**
         * Helper to check if any result is ready and extract that result
         * Synchronized by _resultsMutex
         */
        bool releaseResult_inlock(StatusWith<DBClientCursor*>* nextResult);

        /**
         * Helper to return an error status from zero or more results
         * Synchronized by _resultsMutex
         */
        Status combineErrorResults_inlock();

        // Not owned here
        SystemEnv* _systemEnv;

        // Not owned here
        HostThreadPools* _hostThreads;

        // Outstanding requests
        typedef std::map<ConnectionString, std::shared_ptr<PendingQueryContext> > PendingMap;
        PendingMap _pending;

        // Synchronizes below
        stdx::mutex _resultsMutex;

        // Current results recv'd
        typedef std::map<ConnectionString, StatusWith<DBClientCursor*> > ResultMap;
        ResultMap _results;

        boost::condition_variable _nextResultCV;
    };

    /**
     * Provides network and time services to allow unit testing of MultiHostQueryOp.
     */
    class MultiHostQueryOp::SystemEnv {
    public:

        virtual ~SystemEnv() {
        }

        /**
         * Returns the current time in milliseconds
         */
        virtual Date_t currentTimeMillis() = 0;

        /**
         * Executes a query against a given host.  No timeout hint is given, but the query should
         * not block forever.
         * Note that no guarantees are given as to the state of the connection used after this
         * returns, so the cursor must be self-contained.
         *
         * Caller owns any resulting cursor.
         */
        virtual StatusWith<DBClientCursor*> doBlockingQuery(const ConnectionString& host,
                                                            const QuerySpec& query) = 0;
    };

    /**
     * Object which encapsulates a thread pool per host, and allows scheduling operations against
     * each of these hosts.
     *
     * Optionally supports not waiting for blocked threads before destruction.
     *
     * Thin wrapper for multiple hosts around HostThreadPool.
     */
    class HostThreadPools {
        MONGO_DISALLOW_COPYING(HostThreadPools);
    public:

        typedef stdx::function<void(void)> Callback;

        /**
         * Construct a HostThreadPools object, which lazily constructs thread pools per-host of the
         * specified size.
         *
         * @param scopeAllWork true if the pool should wait for all work to be finished before
         *        going out of scope
         */
        HostThreadPools(int poolSize, bool scopeAllWork);
        ~HostThreadPools();

        /**
         * Schedules some work in the form of a callback for the pool of a particular host.
         */
        void schedule(const ConnectionString& host, Callback callback);

        /**
         * Blocks until pool is idle for a particular host.
         * For testing.
         */
        void waitUntilIdle(const ConnectionString& host);

    private:

        const int _poolSize;
        const bool _scopeAllWork;

        stdx::mutex _mutex;
        typedef std::map<ConnectionString, HostThreadPool*> HostPoolMap;
        HostPoolMap _pools;
    };

    /**
     * EXPOSED FOR TESTING ONLY.
     *
     * Thread pool allowing work to be scheduled against various hosts.
     * Generic interface, but should not be used outside of this class.
     */
    class HostThreadPool {
    public:

        typedef stdx::function<void(void)> Callback;

        /**
         * Constructs a thread pool of a given size.
         *
         * Parameter scopeAllWork indicates whether the pool should wait for all work to be finished
         * before going out of scope.
         */
        HostThreadPool(int poolSize, bool scopeAllWork);

        ~HostThreadPool();

        /**
         * Schedules some work in the form of a callback to be done ASAP.
         */
        void schedule(Callback callback);

        /**
         * Blocks until all threads are idle.
         */
        void waitUntilIdle();

    private:

        /**
         * Synchronized work and activity information shared between the pool and the individual
         * worker threads.
         * This information must be shared, since if !scopeAllWork the parent pool is allowed to
         * fall out of scope before the child thread completes.
         */
        struct PoolContext {

            PoolContext() :
                numActiveWorkers(0), isPoolActive(true) {
            }

            // Synchronizes below
            stdx::mutex mutex;

            // The scheduled work
            std::deque<Callback> scheduled;
            boost::condition_variable workScheduledCV;

            // How many workers are currently active
            int numActiveWorkers;
            boost::condition_variable isIdleCV;

            // Whether the pool has been disposed of
            bool isPoolActive;
        };

        /**
         * Worker loop run by each thread.
         */
        static void doWork(std::shared_ptr<PoolContext> context);

        const bool _scopeAllWork;

        // For now, only modified in the constructor and destructor, but non-const
        std::vector<boost::thread*> _threads;

        // Shared work and worker activity information
        std::shared_ptr<PoolContext> _context;
    };
}
