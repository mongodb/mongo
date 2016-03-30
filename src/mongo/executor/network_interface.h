/**
 *    Copyright (C) 2014-2015 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class BSONObjBuilder;

namespace executor {

/**
 * Interface to networking for use by TaskExecutor implementations.
 */
class NetworkInterface {
    MONGO_DISALLOW_COPYING(NetworkInterface);

public:
    // A flag to keep replication MessagingPorts open when all other sockets are disconnected.
    static const unsigned int kMessagingPortKeepOpen = 1;

    using Response = RemoteCommandResponse;
    using RemoteCommandCompletionFn = stdx::function<void(const TaskExecutor::ResponseStatus&)>;

    virtual ~NetworkInterface();

    /**
     * Returns diagnostic info.
     */
    virtual std::string getDiagnosticString() = 0;

    /**
     * Appends information about the connections on this NetworkInterface.
     */
    virtual void appendConnectionStats(ConnectionPoolStats* stats) const = 0;

    /**
     * Starts up the network interface.
     *
     * It is valid to call all methods except shutdown() before this method completes.  That is,
     * implementations may not assume that startup() completes before startCommand() first
     * executes.
     *
     * Called by the owning TaskExecutor inside its run() method.
     */
    virtual void startup() = 0;

    /**
     * Shuts down the network interface. Must be called before this instance gets deleted,
     * if startup() is called.
     *
     * Called by the owning TaskExecutor inside its run() method.
     */
    virtual void shutdown() = 0;

    /**
     * Returns true if shutdown has been called, false otherwise.
     */
    virtual bool inShutdown() const = 0;

    /**
     * Blocks the current thread (presumably the executor thread) until the network interface
     * knows of work for the executor to perform.
     */
    virtual void waitForWork() = 0;

    /**
     * Similar to waitForWork, but only blocks until "when".
     */
    virtual void waitForWorkUntil(Date_t when) = 0;

    /**
     * Signals to the network interface that there is new work (such as a signaled event) for
     * the executor to process.  Wakes the executor from waitForWork() and friends.
     */
    virtual void signalWorkAvailable() = 0;

    /**
     * Returns the current time.
     */
    virtual Date_t now() = 0;

    /**
     * Returns the hostname of the current process.
     */
    virtual std::string getHostName() = 0;

    /**
     * Starts asynchronous execution of the command described by "request".
     *
     * Returns ErrorCodes::ShutdownInProgress if NetworkInterface::shutdown has already started
     * and Status::OK() otherwise. If it returns Status::OK(), then the onFinish argument will be
     * executed by NetworkInterface eventually; otherwise, it will not.
     */
    virtual Status startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                const RemoteCommandRequest& request,
                                const RemoteCommandCompletionFn& onFinish) = 0;

    /**
     * Requests cancelation of the network activity associated with "cbHandle" if it has not yet
     * completed.
     */
    virtual void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) = 0;

    /**
     * Requests cancelation of incomplete network activity.
     */
    virtual void cancelAllCommands() = 0;

    /**
     * Sets an alarm, which schedules "action" to run no sooner than "when".
     *
     * Returns ErrorCodes::ShutdownInProgress if NetworkInterface::shutdown has already started
     * and true otherwise. If it returns Status::OK(), then the action will be executed by
     * NetworkInterface eventually; otherwise, it will not.
     *
     * "action" should not do anything that requires a lot of computation, or that might block for a
     * long time, as it may execute in a network thread.
     *
     * Any callbacks invoked from setAlarm must observe onNetworkThread to
     * return true. See that method for why.
     */
    virtual Status setAlarm(Date_t when, const stdx::function<void()>& action) = 0;

    /**
     * Returns true if called from a thread dedicated to networking. I.e. not a
     * calling thread.
     *
     * This is meant to be used to avoid context switches, so callers must be
     * able to rely on this returning true in a callback or completion handler.
     * In the absence of any actual networking thread, always return true.
     */
    virtual bool onNetworkThread() = 0;

protected:
    NetworkInterface();
};

}  // namespace executor
}  // namespace mongo
