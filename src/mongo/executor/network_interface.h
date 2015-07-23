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
     * An interface for augmenting the NetworkInterface with domain-specific host validation and
     * post-connection logic.
     */
    class ConnectionHook {
    public:
        virtual ~ConnectionHook() = default;

        /**
         * Runs optional validation logic on an isMaster reply from a remote host. If a non-OK
         * Status is returned, it will be propagated up to the completion handler for the command
         * that initiated the request that caused this connection to be created. This will
         * be called once for each connection that is created, even if a remote host with the
         * same HostAndPort has already successfully passed validation on a different connection.
         *
         * This method must not throw any exceptions or block on network or disk-IO. However, in the
         * event that an exception escapes, the NetworkInterface is responsible for calling
         * std::terminate.
         */
        virtual Status validateHost(const HostAndPort& remoteHost,
                                    const RemoteCommandResponse& isMasterReply) = 0;

        /**
         * Generates a command to run on the remote host immediately after connecting to it.
         * If a non-OK StatusWith is returned, it will be propagated up to the completion handler
         * for the command that initiated the request that caused this connection to be created.
         *
         * The command will be run after socket setup, SSL handshake, authentication, and wire
         * protocol detection, but before any commands submitted to the NetworkInterface via
         * startCommand are run. In the case that it isn't neccessary to run a command, makeRequest
         * may return boost::none.
         *
         * This method must not throw any exceptions or block on network or disk-IO. However, in the
         * event that an exception escapes, the NetworkInterface is responsible for calling
         * std::terminate.
         */
        virtual StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
            const HostAndPort& remoteHost) = 0;

        /**
         * Handles a remote server's reply to the command generated with makeRequest. If a
         * non-OK Status is returned, it will be propagated up to the completion handler for the
         * command that initiated the request that caused this connection to be created.
         *
         * If the corresponding earlier call to makeRequest for this connection returned
         * boost::none, the NetworkInterface will not call handleReply.
         *
         * This method must not throw any exceptions or block on network or disk-IO. However, in the
         * event that an exception escapes, the NetworkInterface is responsible for calling
         * std::terminate.
         */
        virtual Status handleReply(const HostAndPort& remoteHost,
                                   RemoteCommandResponse&& response) = 0;

    protected:
        ConnectionHook() = default;
    };

    /**
     * Returns diagnostic info.
     */
    virtual std::string getDiagnosticString() = 0;

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
     * Blocks the current thread (presumably the executor thread) until the network interface
     * knows of work for the executor to perform.
     */
    virtual void waitForWork() = 0;

    /**
     * Similar to waitForWork, but only blocks until "when".
     */
    virtual void waitForWorkUntil(Date_t when) = 0;

    /**
     * Sets a connection hook for this NetworkInterface. This method can only be
     * called once, and must be called before startup() or the result
     * is undefined.
     */
    virtual void setConnectionHook(std::unique_ptr<ConnectionHook> hook) = 0;

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
     */
    virtual void startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                              const RemoteCommandRequest& request,
                              const RemoteCommandCompletionFn& onFinish) = 0;

    /**
     * Requests cancelation of the network activity associated with "cbHandle" if it has not yet
     * completed.
     */
    virtual void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) = 0;

    /**
     * Sets an alarm, which schedules "action" to run no sooner than "when".
     *
     * "action" should not do anything that requires a lot of computation, or that might block for a
     * long time, as it may execute in a network thread.
     */
    virtual void setAlarm(Date_t when, const stdx::function<void()>& action) = 0;

protected:
    NetworkInterface();
};

}  // namespace executor
}  // namespace mongo
