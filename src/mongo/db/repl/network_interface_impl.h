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

#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <vector>

#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/list.h"

namespace mongo {
namespace repl {

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

    private:
        struct CommandData {
            ReplicationExecutor::CallbackHandle cbHandle;
            ReplicationExecutor::RemoteCommandRequest request;
            RemoteCommandCompletionFn onFinish;
        };
        typedef stdx::list<CommandData> CommandDataList;
        typedef std::vector<boost::shared_ptr<boost::thread> > ThreadList;

        void _consumeNetworkRequests();
        ResponseStatus _runCommand(const ReplicationExecutor::RemoteCommandRequest& request);
        void _signalWorkAvailable_inlock();

        boost::mutex _mutex;
        boost::condition_variable _hasPending;
        CommandDataList _pending;
        ThreadList _threads;
        boost::condition_variable _isExecutorRunnableCondition;
        bool _isExecutorRunnable;
        bool _inShutdown;
    };

}  // namespace repl
} // namespace mongo
