/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <atomic>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_mode.h"

namespace mongo {

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
     * Creates a new ServiceStateMachine for a given session/service context. If sync is true,
     * then calls into the transport layer will block while they complete, otherwise they will
     * be handled asynchronously.
     */
    static std::shared_ptr<ServiceStateMachine> create(ServiceContext* svcContext,
                                                       transport::SessionHandle session,
                                                       transport::Mode transportMode);

    ServiceStateMachine(ServiceContext* svcContext,
                        transport::SessionHandle session,
                        transport::Mode transportMode);

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
     * runNext() will run the current state of the state machine. It also handles all the error
     * handling and state management for requests.
     *
     * Each state function (processMessage(), sinkCallback(), etc) should always unwind the stack
     * if they have just completed a database operation to make sure that this doesn't infinitely
     * recurse.
     *
     * runNext() will attempt to create a ThreadGuard when it first runs. If it's unable to take
     * ownership of the SSM, it will call scheduleNext() and return immediately.
     */
    void runNext();

    /*
     * start() schedules a call to runNext() in the future.
     *
     * It is guaranteed to unwind the stack, and not call runNext() recursively, but is not
     * guaranteed that runNext() will run after this return
     */
    void start(Ownership ownershipModel);

    /*
     * Set the executor to be used for the next call to runNext(). This allows switching between
     * thread models after the SSM has started.
     */
    void setServiceExecutor(transport::ServiceExecutor* executor);

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
    void setCleanupHook(stdx::function<void()> hook);

private:
    /*
     * A class that wraps up lifetime management of the _dbClient and _threadName for runNext();
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
                                transport::ServiceExecutorTaskName taskName,
                                Ownership ownershipModel = Ownership::kOwned);

    /*
     * Gets the transport::Session associated with this connection
     */
    const transport::SessionHandle& _session() const;

    /*
     * This is the actual implementation of runNext() that gets called after the ThreadGuard
     * has been successfully created. If any callbacks (like sourceCallback()) need to call
     * runNext() and already own a ThreadGuard, they should call this with that guard as the
     * argument.
     */
    void _runNextInGuard(ThreadGuard guard);

    /*
     * This function actually calls into the database and processes a request. It's broken out
     * into its own inline function for better readability.
     */
    inline void _processMessage(ThreadGuard guard);

    /*
     * These get called by the TransportLayer when requested network I/O has completed.
     */
    void _sourceCallback(Status status);
    void _sinkCallback(Status status);

    /*
     * Source/Sink message from the TransportLayer. These will invalidate the ThreadGuard just
     * before waiting on the TL.
     */
    void _sourceMessage(ThreadGuard guard);
    void _sinkMessage(ThreadGuard guard, Message toSink);

    /*
     * Releases all the resources associated with the session and call the cleanupHook.
     */
    void _cleanupSession(ThreadGuard guard);

    AtomicWord<State> _state{State::Created};

    ServiceEntryPoint* _sep;
    transport::Mode _transportMode;

    ServiceContext* const _serviceContext;
    transport::ServiceExecutor* _serviceExecutor;

    transport::SessionHandle _sessionHandle;
    const std::string _threadName;
    ServiceContext::UniqueClient _dbClient;
    const Client* _dbClientPtr;
    stdx::function<void()> _cleanupHook;

    bool _inExhaust = false;
    boost::optional<MessageCompressorId> _compressorId;
    Message _inMessage;

    AtomicWord<Ownership> _owned{Ownership::kUnowned};
#if MONGO_CONFIG_DEBUG_BUILD
    AtomicWord<stdx::thread::id> _owningThread;
#endif
    std::string _oldThreadName;
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

}  // namespace mongo
