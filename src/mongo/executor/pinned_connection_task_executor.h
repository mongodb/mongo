/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo::executor {

/**
 * Implementation of a TaskExecutor that provides the ability to schedule RPC/networking on the same
 * underlying network connection. The PinnedTaskExecutor is constructed from another TaskExecutor,
 * and uses that TaskExecutor's ThreadPool and NetworkInterface/networking reactor to perform work.
 * Specifically:
 * - Functions that schedule work or manage events that happen locally, without going over the
 *   network, are passed-through to the underlying TaskExecutor (i.e. scheduleWork,
 *   makeEvent, waitForEvent).
 * - Functions that involve scheduling RPC/networking are all run on the same underlying
 *   network-connection (i.e. TCP/Unix Domain Socket).
 * Note that this means that the PinnedConnectionTaskExecutor can only speak to one host over its
 * entire lifetime! If you need to speak to a different host, you need a different connection, so
 * construct a *new* PinnedCursorTaskExecutor from the underlying executor.
 *
 * Certain methods are illegal to call. startup() is illegal to call because the TaskExecutor
 * passed to PinnedConnectionTaskExecutor should be started-up prior to this object's construction,
 * and no additional startup is needed.
 * Additionally, diagnostic and network management methods:
 * - appendDiagnosticBSON()
 * - appendConnectionStats()
 * - dropConnections()
 * - appendNetworkInterfaceStats()
 * are illegal to call because this TaskExecutor provides a distinct networking API. Gather
 * diagnostics from the underlying TaskExecutor instead if needed.
 *
 * This type uses ScopedTaskExecutor to proxy work to the underlying TaskExecutor it is
 * constructed from. This means that shutdown() and join() address only tasks dispatched
 * through this executor, rather than passing through to the underlying executor.
 *
 * Note! The executor that this PinnedConnectionTaskExecutor is constructed from _must_
 * out-life it - i.e. this PinnedConnectionTaskExecutor must be shutdown and joined
 * before the underlying executor is. This is because this type must have access
 * to the underlying thread pool to complete cancellation tasks as it shuts down.
 *
 * Exhaust commands are not supported at this time.
 */
class PinnedConnectionTaskExecutor final : public TaskExecutor {
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    static std::shared_ptr<PinnedConnectionTaskExecutor> create(
        std::shared_ptr<TaskExecutor> executor, NetworkInterface* net) {
        return std::make_shared<PinnedConnectionTaskExecutor>(Passkey{}, std::move(executor), net);
    }

    // The provided NetworkInterface should be owned by the provided TaskExecutor, and
    // must outlive this type.
    PinnedConnectionTaskExecutor(Passkey,
                                 const std::shared_ptr<TaskExecutor>& executor,
                                 NetworkInterface* net);

    ~PinnedConnectionTaskExecutor() override;

    PinnedConnectionTaskExecutor(const PinnedConnectionTaskExecutor&) = delete;
    PinnedConnectionTaskExecutor& operator=(const PinnedConnectionTaskExecutor&) = delete;

    // Startup is illegal to call, as the provided executor should already be started-up.
    void startup() override;
    void shutdown() override;
    void join() override;
    SharedSemiFuture<void> joinAsync() override;
    bool isShuttingDown() const override;

    // These pass-through to the underlying TaskExecutor.
    Date_t now() override;
    StatusWith<EventHandle> makeEvent() override;
    void signalEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> onEvent(const EventHandle& event, CallbackFn&& work) override;
    void waitForEvent(const EventHandle& event) override;
    StatusWith<stdx::cv_status> waitForEvent(OperationContext* opCtx,
                                             const EventHandle& event,
                                             Date_t deadline) override;
    StatusWith<CallbackHandle> scheduleWork(CallbackFn&& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, CallbackFn&& work) override;

    // This type provides special connection-pinning behavior for RPC functionality here.
    StatusWith<CallbackHandle> scheduleRemoteCommand(const RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override;

    StatusWith<CallbackHandle> scheduleExhaustRemoteCommand(
        const RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) override;

    // When cancel() is passed a CallbackHandle that was returned from schedule{Work}()/onEvent(),
    // cancellation is passed-through to the underlying executor. If the CallbackHandle was returned
    // from scheduleRemoteCommand then the executor will cancel the RPC attempt.
    void cancel(const CallbackHandle& cbHandle) override;

    // Wait is unimplemented at this time.
    void wait(const CallbackHandle& cbHandle,
              Interruptible* interruptible = Interruptible::notInterruptible()) override;

    // Illegal to call because the view does not track it's portion of the underlying TaskExecutor's
    // resources.
    void appendConnectionStats(ConnectionPoolStats*) const override;
    void appendNetworkInterfaceStats(BSONObjBuilder&) const override;
    void appendDiagnosticBSON(BSONObjBuilder*) const override;
    void dropConnections(const HostAndPort&, const Status&) override;
    bool hasTasks() override;

private:
    // Ensures _stream is initialized with a valid LeasedStream to `target`.
    // If we already have a _stream when this function is called, ensures the
    // remote is `target` and returns a ready-future. Otherwise asynchronously
    // initailizes _stream and returns a future that resolves once _stream is ready.
    ExecutorFuture<void> _ensureStream(WithLock,
                                       HostAndPort target,
                                       Milliseconds timeout,
                                       transport::ConnectSSLMode sslMode);

    // Start processing pending/queued RPCs.
    void _doNetworking(stdx::unique_lock<stdx::mutex>&&);

    // CallbackState for RPCs. Non-RPC callbacks use the CallbackState from the _underlyingExecutor.
    class CallbackState;

    // Invoke the RPC and return a future of its response.
    Future<RemoteCommandResponse> _runSingleCommand(RemoteCommandRequest command,
                                                    std::shared_ptr<CallbackState> cbState);

    void _shutdown(WithLock);

    // Alias for an RPC request and the associated CallbackState.
    using RequestAndCallback = std::pair<RemoteCommandRequest, std::shared_ptr<CallbackState>>;

    // Helper to cancel a CallbackState from this executor.
    void _cancel(WithLock, CallbackState*);

    // Helper that walks the _requestQueue in-order, completing any canceled callbacks, until
    // it finds the first uncanceled one (if any), which it returns.
    boost::optional<RequestAndCallback> _getFirstUncanceledRequest(stdx::unique_lock<stdx::mutex>&);

    // Synchronizes access to the _requestQueue, _stream, and _isDoingNetworking variables, as well
    // as all CallbackState members.
    mutable stdx::mutex _mutex;

    ScopedTaskExecutor _executor;
    // Owned by the TaskExecutor backing _executor above. Since ScopedTaskExecutor keeps a
    // shared_ptr to it's backing TaskExecutor, _net will remain valid for at least the lifetime of
    // _executor.
    NetworkInterface* _net;

    // This is the same executor that the ScopedTaskExecutor above provides a view over. We keep
    // a pointer to it so that we can run cancellation tasks even after the ScopedTaskExecutor
    // is shut down. This should _only_ be used to guarantee cancellation tasks will run, even
    // after shutdown is called on this type!
    std::shared_ptr<TaskExecutor> _cancellationExecutor;

    // Queue of pending/not-yet-started RPC requests and corresponding completion callbacks
    // scheduled on this executor.
    std::deque<RequestAndCallback> _requestQueue;
    stdx::condition_variable _requestQueueEmptyCV;
    // Pinned-connection leased from _underlyingNet to run all RPCs through this executor.
    // Initialized upon the execution of the first scheduled RPC, and subsequently re-used for all
    // RPCs scheduled through this executor.
    std::unique_ptr<NetworkInterface::LeasedStream> _stream;
    bool _isDoingNetworking{false};
    std::shared_ptr<CallbackState> _inProgressRequest;

    enum class State { running, joinRequired, joining, shutdownComplete };
    State _state = State::running;
};

}  // namespace mongo::executor
