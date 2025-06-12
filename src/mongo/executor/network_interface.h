/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/async_client.h"
#include "mongo/db/baton.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace executor {

extern FailPoint networkInterfaceHangCommandsAfterAcquireConn;
extern FailPoint networkInterfaceCommandsFailedWithErrorCode;
extern FailPoint networkInterfaceShouldNotKillPendingRequests;

/**
 * Interface to networking for use by TaskExecutor implementations.
 */
class NetworkInterface {
    NetworkInterface(const NetworkInterface&) = delete;
    NetworkInterface& operator=(const NetworkInterface&) = delete;

public:
    using Response = RemoteCommandResponse;
    /**
     * This must not throw exceptions.
     */
    using RemoteCommandCompletionFn = unique_function<void(const TaskExecutor::ResponseStatus&)>;
    /**
     * This must not throw exceptions.
     */
    using RemoteCommandOnReplyFn = unique_function<void(const TaskExecutor::ResponseStatus&)>;

    // Indicates that there is no expiration time by when a request needs to complete
    static constexpr Date_t kNoExpirationDate{Date_t::max()};

    /**
     * Debug log level which includes all debug logging emitted by implementations of
     * NetworkInterface. Tests for NetworkInterface should ensure the kNetwork component's log
     * verbosity is set to this debug level.
     */
    static constexpr int kDiagnosticLogLevel = 4;

    class ExhaustResponseReader : public std::enable_shared_from_this<ExhaustResponseReader> {
    public:
        ExhaustResponseReader(const ExhaustResponseReader&) = delete;
        ExhaustResponseReader& operator=(const ExhaustResponseReader&) = delete;

        virtual ~ExhaustResponseReader() = default;

        /**
         * Reads the next response from the exhaust command.
         *
         * The returned future is always successful, though the status contained in the
         * RemoteCommandResponse may not be OK.
         *
         * If the returned Future is fulfilled with a response whose moreToCome member is false,
         * then no further responses will be available for reading. Any subsequent attempt to read a
         * response will return a Future fulfilled with a RemoteCommandResponse containing an
         * ExhaustCommandFinished error.
         *
         * If the startExhaustCommand invocation that created this used a baton, calls to next()
         * will also use that baton.
         *
         * Similarly, cancelling the source associated with the CancellationToken that was
         * originally passed to the the startExhaustCommand invocation will cancel this
         * ExhaustResponseReader and cause subsequent calls to next() to return futures fulfilled
         * with responses containing CallbackCancelled errors.
         *
         * The timeout provided to the originating RemoteCommandRequest will be used for each call
         * to next().
         */
        virtual SemiFuture<RemoteCommandResponse> next() = 0;

    protected:
        ExhaustResponseReader() = default;
    };

    virtual ~NetworkInterface();

    /**
     * Returns diagnostic info.
     */
    virtual std::string getDiagnosticString() = 0;

    /**
     * Appends information about the connections on this NetworkInterface.
     * TODO SERVER-100677: Remove
     */
    virtual void appendConnectionStats(ConnectionPoolStats* stats) const = 0;

    /**
     * Appends information about this instance of NetworkInterface.
     */
    virtual void appendStats(BSONObjBuilder&) const = 0;

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

    struct Counters {
        uint64_t sent = 0;
        uint64_t canceled = 0;
        uint64_t timedOut = 0;
        uint64_t failed = 0;
        uint64_t failedRemotely = 0;
        uint64_t succeeded = 0;
    };
    /*
     * Returns a copy of the operation counters (see struct Counters above). This method should
     * only be used in tests, and will invariant if testing diagnostics are not enabled.
     */
    virtual Counters getCounters() const = 0;

    /**
     * Starts asynchronous execution of the command described by "request".
     *
     * The provided request will be mutated to include additional metadata.
     *
     * This may throw exceptions if a command is unable to be scheduled.
     */
    virtual SemiFuture<TaskExecutor::ResponseStatus> startCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest& request,
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable()) = 0;

    /**
     * Starts asynchronous execution of the exhaust command described by "request", returning an
     * ExhaustResponseReader which may be used to receive responses pushed by the remote.
     *
     * The provided request will be mutated to include additional metadata.
     *
     * This may throw exceptions if a command is unable to be scheduled.
     *
     * The cancellation token passed to this method will be forwarded to the returned
     * ExhaustResponseReader, so the source that produced that token can be used to cancel any
     * requests made by the ExhaustResponseReader. Once the source has been canceled, the entire
     * exhaust command is canceled and no further responses can be read.
     */
    virtual SemiFuture<std::shared_ptr<NetworkInterface::ExhaustResponseReader>>
    startExhaustCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest& request,
        const BatonHandle& baton = nullptr,
        const CancellationToken& cancelToken = CancellationToken::uncancelable()) = 0;

    /**
     * Requests cancellation of the network activity associated with "cbHandle" if it has not yet
     * completed.
     *
     * Note that the work involved in onFinish may run locally as a result of invoking this
     * function. Do not hold locks while calling cancelCommand(...).
     */
    virtual void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                               const BatonHandle& baton = nullptr) = 0;

    /**
     * Sets an alarm, which fulfills the returned Future no sooner than "when".
     *
     * Ready immediately with ErrorCodes::ShutdownInProgress if NetworkInterface::shutdown has
     * already started.
     */
    virtual SemiFuture<void> setAlarm(
        Date_t when, const CancellationToken& token = CancellationToken::uncancelable()) = 0;

    /**
     * Schedules the specified action to run as soon as possible on the network interface's
     * execution resource
     */
    virtual Status schedule(unique_function<void(Status)> action) = 0;

    /**
     * Returns true if called from a thread dedicated to networking. I.e. not a
     * calling thread.
     *
     * This is meant to be used to avoid context switches, so callers must be
     * able to rely on this returning true in a callback or completion handler.
     * In the absence of any actual networking thread, always return true.
     */
    virtual bool onNetworkThread() = 0;

    /**
     * Drops all connections to the given host in the connection pool.
     */
    virtual void dropConnections(const HostAndPort& target, const Status& status) = 0;

    /**
     * Acquire a connection and subsequently release it.
     * If status is not OK, the connection will be dropped,
     * otherwise the connection will be returned to the pool.
     * Throws `ExceptionFor<ErrorCodes::NotYetInitialized>`
     * if NetworkInterface is not started up yet.
     */
    virtual void testEgress(const HostAndPort& hostAndPort,
                            transport::ConnectSSLMode sslMode,
                            Milliseconds timeout,
                            Status status) = 0;

    /**
     * An RAII type that NetworkInterface uses to allow users to access a stream corresponding to a
     * single network-level (i.e. TCP, UDS) connection on which to run commands directly. Generally
     * speaking, users should use the NetworkInterface itself to run commands to take advantage of
     * connection-pooling, hedging, and other features - but for special cases where users need to
     * borrow their own network-stream for manual use, they can lease one through this type.
     * LeasedStreams are minimal and do not offer automated health-management/automated refreshing
     * while on lease - users are responsible for examining the health of the stream as-needed if
     * they desire. Users are also responsible for reporting on the health of stream before the
     * lease ends so that it can be subsequently re-used; see comments below for detail.
     */
    class LeasedStream {
    public:
        virtual ~LeasedStream() = default;

        // AsyncDBClient provides the mongoRPC-API for running commands over this stream. This
        // stream owns the AsyncDBClient and no outstanding networking should be scheduled on the
        // client when it is destroyed.
        virtual AsyncDBClient* getClient() = 0;

        // Indicates that the user is done with this leased stream, and no failures on it occured.
        // Users MUST call either this function or indicateFailure before the LeasedStream is
        // destroyed.
        virtual void indicateSuccess() = 0;
        // Indicates that the stream is unhealthy (i.e. the user received a network error indicating
        // the stream failed). This prevents the stream from being reused.
        virtual void indicateFailure(Status) = 0;
        // Indicates that the stream has successfully performed networking over the stream. Updates
        // metadata indicating the last healthy networking over the stream so that appropriate
        // health-checks can be done after the lease ends.
        virtual void indicateUsed() = 0;
    };

    /**
     * Lease a stream from this NetworkInterface for manual use.
     */
    virtual SemiFuture<std::unique_ptr<LeasedStream>> leaseStream(const HostAndPort& hostAndPort,
                                                                  transport::ConnectSSLMode sslMode,
                                                                  Milliseconds timeout) = 0;

protected:
    NetworkInterface();
};

}  // namespace executor
}  // namespace mongo
