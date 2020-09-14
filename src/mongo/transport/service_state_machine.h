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

#include <atomic>
#include <functional>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_mode.h"
#include "mongo/util/future.h"
#include "mongo/util/net/ssl_manager.h"

namespace mongo {
namespace transport {

/*
 * The ServiceStateMachine holds the state of a single client connection and represents the
 * lifecycle of each user request as a state machine. It is the glue between the stateless
 * ServiceEntryPoint and TransportLayer that ties network and database logic together for a
 * user.
 */
class ServiceStateMachine : public std::enable_shared_from_this<ServiceStateMachine> {
    ServiceStateMachine(ServiceStateMachine&) = delete;
    ServiceStateMachine& operator=(ServiceStateMachine&) = delete;

public:
    ServiceStateMachine(ServiceStateMachine&&) = delete;
    ServiceStateMachine& operator=(ServiceStateMachine&&) = delete;

    /*
     * Construct a ServiceStateMachine for a given Client.
     */
    ServiceStateMachine(ServiceContext::UniqueClient client);

    /*
     * Any state may transition to EndSession in case of an error, otherwise the valid state
     * transitions are:
     * Source -> SourceWait -> Process -> SinkWait -> Source (standard RPC)
     * Source -> SourceWait -> Process -> SinkWait -> Process -> SinkWait ... (exhaust)
     * Source -> SourceWait -> Process -> Source (fire-and-forget)
     */
    enum class State {
        Created,     // The session has been created, but no operations have been performed yet
        Source,      // Request a new Message from the network to handle
        SourceWait,  // Wait for the new Message to arrive from the network
        Process,     // Run the Message through the database
        SinkWait,    // Wait for the database result to be sent by the network
        EndSession,  // End the session - the ServiceStateMachine will be invalid after this
        Ended        // The session has ended. It is illegal to call any method besides
                     // state() if this is the current state.
    };

    /*
     * When start() is called with Ownership::kOwned, the SSM will swap the Client/thread name
     * whenever it runs a stage of the state machine, and then unswap them out when leaving the SSM.
     *
     * With Ownership::kStatic, it will assume that the SSM will only ever be run from one thread,
     * and that thread will not be used for other SSM's. It will swap in the Client/thread name
     * for the first run and leave them in place.
     *
     * kUnowned is used internally to mark that the SSM is inactive.
     */
    enum class Ownership { kUnowned, kOwned, kStatic };

    /*
     * start() schedules a call to _runOnce() in the future.
     */
    void start();

    /*
     * Gets the current state of connection for testing/diagnostic purposes.
     */
    State state();

    /*
     * Terminates the associated transport Session, regardless of tags.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminate();

    /*
     * Terminates the associated transport Session if its tags don't match the supplied tags.
     * If the session is in a pending state, before any tags have been set, it will not be
     * terminated.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminateIfTagsDontMatch(transport::Session::TagMask tags);

    /*
     * Sets a function to be called after the session is ended
     */
    void setCleanupHook(std::function<void()> hook);

private:
    /*
     * A class that wraps up lifetime management of the _dbClient and _threadName for
     * each step in _runOnce();
     */
    class ThreadGuard;
    friend class ThreadGuard;

    /*
     * Terminates the associated transport Session if status indicate error.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void _terminateAndLogIfError(Status status);

    /*
     * This is a helper function to schedule tasks on the serviceExecutor maintaining a shared_ptr
     * copy to anchor the lifetime of the SSM while waiting for callbacks to run.
     *
     * If scheduling the function fails, the SSM will be terminated and cleaned up immediately
     */
    void _scheduleNextWithGuard(ThreadGuard guard,
                                transport::ServiceExecutor::ScheduleFlags flags,
                                Ownership ownershipModel = Ownership::kOwned);

    /*
     * Gets the transport::Session associated with this connection
     */
    const transport::SessionHandle& _session() const;

    /*
     * Gets the transport::ServiceExecutor associated with this connection.
     */
    ServiceExecutor* _executor();

    /*
     * This function actually calls into the database and processes a request. It's broken out
     * into its own inline function for better readability.
     */
    Future<void> _processMessage(ThreadGuard guard);

    /*
     * These get called by the TransportLayer when requested network I/O has completed.
     */
    void _sourceCallback(Status status);
    void _sinkCallback(Status status);

    /*
     * Source/Sink message from the TransportLayer. These will invalidate the ThreadGuard just
     * before waiting on the TL.
     */
    Future<void> _sourceMessage(ThreadGuard guard);
    Future<void> _sinkMessage(ThreadGuard guard);

    /*
     * Releases all the resources associated with the session and call the cleanupHook.
     */
    void _cleanupSession(ThreadGuard guard);

    /*
     * This is the initial function called at the beginning of a thread's lifecycle in the
     * TransportLayer.
     */
    void _runOnce();

    /*
     * Releases all the resources associated with the exhaust request.
     */
    void _cleanupExhaustResources() noexcept;

    AtomicWord<State> _state{State::Created};

    ServiceContext* const _serviceContext;

    transport::SessionHandle _sessionHandle;
    ServiceContext::UniqueClient _dbClient;
    const Client* _dbClientPtr;
    transport::ServiceExecutor* _serviceExecutor;
    std::function<void()> _cleanupHook;

    bool _inExhaust = false;
    boost::optional<MessageCompressorId> _compressorId;
    Message _inMessage;
    Message _outMessage;

    // Allows delegating destruction of opCtx to another function to potentially remove its cost
    // from the critical path. This is currently only used in `_processMessage()`.
    ServiceContext::UniqueOperationContext _killedOpCtx;

    AtomicWord<Ownership> _owned{Ownership::kUnowned};
#if MONGO_CONFIG_DEBUG_BUILD
    AtomicWord<stdx::thread::id> _owningThread;
#endif
};

template <typename T>
T& operator<<(T& stream, const ServiceStateMachine::State& state) {
    switch (state) {
        case ServiceStateMachine::State::Created:
            stream << "created";
            break;
        case ServiceStateMachine::State::Source:
            stream << "source";
            break;
        case ServiceStateMachine::State::SourceWait:
            stream << "sourceWait";
            break;
        case ServiceStateMachine::State::Process:
            stream << "process";
            break;
        case ServiceStateMachine::State::SinkWait:
            stream << "sinkWait";
            break;
        case ServiceStateMachine::State::EndSession:
            stream << "endSession";
            break;
        case ServiceStateMachine::State::Ended:
            stream << "ended";
            break;
    }
    return stream;
}

}  // namespace transport
}  // namespace mongo
