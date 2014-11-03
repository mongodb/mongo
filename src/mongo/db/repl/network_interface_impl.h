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

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <vector>

#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/list.h"

namespace mongo {
namespace repl {

    /**
     * Implementation of the network interface used by the ReplicationExecutor inside mongod.
     */
    class NetworkInterfaceImpl : public ReplicationExecutor::NetworkInterface {
    public:
        explicit NetworkInterfaceImpl();
        virtual ~NetworkInterfaceImpl();
        virtual void startup();
        virtual void shutdown();
        virtual void waitForWork();
        virtual void waitForWorkUntil(Date_t when);
        virtual void signalWorkAvailable();
        virtual Date_t now();
        virtual void startCommand(
                const ReplicationExecutor::CallbackHandle& cbHandle,
                const ReplicationExecutor::RemoteCommandRequest& request,
                const RemoteCommandCompletionFn& onFinish);
        virtual void cancelCommand(const ReplicationExecutor::CallbackHandle& cbHandle);
        virtual void runCallbackWithGlobalExclusiveLock(
                const stdx::function<void (OperationContext*)>& callback);

        std::string getNextCallbackWithGlobalLockThreadName();

    private:
        class ConnectionPool;

        /**
         * Information describing an in-flight command.
         */
        struct CommandData {
            ReplicationExecutor::CallbackHandle cbHandle;
            ReplicationExecutor::RemoteCommandRequest request;
            RemoteCommandCompletionFn onFinish;
        };
        typedef stdx::list<CommandData> CommandDataList;
        typedef std::vector<boost::shared_ptr<boost::thread> > ThreadList;

        /**
         * Thread body for threads that synchronously perform network requests from
         * the _pending list.
         */
        void _consumeNetworkRequests(const std::string& threadName);

        /**
         * Synchronously invokes the command described by "request".
         */
        ResponseStatus _runCommand(const ReplicationExecutor::RemoteCommandRequest& request);

        /**
         * Notifies the network threads that there is work available.
         */
        void _signalWorkAvailable_inlock();

        // Mutex guarding the state of the network interface, except for the pool pointed to by
        // _connPool.
        boost::mutex _mutex;

        // Condition signaled to indicate that there is work in the _pending queue.
        boost::condition_variable _hasPending;

        // Queue of yet-to-be-executed network operations.
        CommandDataList _pending;

        // List of threads serving as the worker pool.
        ThreadList _threads;

        // Condition signaled to indicate that the executor, blocked in waitForWorkUntil or
        // waitForWork, should wake up.
        boost::condition_variable _isExecutorRunnableCondition;

        // Flag indicating whether or not the executor associated with this interface is runnable.
        bool _isExecutorRunnable;

        // Flag indicating when this interface is being shut down (because shutdown() has executed).
        bool _inShutdown;

        // Pool of connections to remote nodes, used by the worker threads to execute network
        // requests.
        boost::scoped_ptr<ConnectionPool> _connPool;  // (R)

    };

}  // namespace repl
} // namespace mongo
