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
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"

namespace mongo {
class ServiceEntryPoint;

namespace transport {
class ServiceExecutorBase;
}  // namespace transport

/*
 * The ServiceStateMachine holds the state of a single client connection and represents the
 * lifecycle of each user request as a state machine. It is the glue between the stateless
 * ServiceEntryPoint and TransportLayer that ties network and database logic together for a
 * user.
 */
class ServiceStateMachine {
    ServiceStateMachine(ServiceStateMachine&) = delete;
    ServiceStateMachine& operator=(ServiceStateMachine&) = delete;

public:
    ServiceStateMachine() = default;
    ServiceStateMachine(ServiceStateMachine&&) = default;
    ServiceStateMachine& operator=(ServiceStateMachine&&) = default;

    ServiceStateMachine(ServiceContext* svcContext, transport::SessionHandle session, bool sync);

    /*
     * Any state may transition to EndSession in case of an error, otherwise the valid state
     * transitions are:
     * Source -> SourceWait -> Process -> SinkWait -> Source (standard RPC)
     * Source -> SourceWait -> Process -> SinkWait -> Process -> SinkWait ... (exhaust)
     * Source -> SourceWait -> Process -> Source (fire-and-forget)
     */
    enum class State {
        Source,      // Request a new Message from the network to handle
        SourceWait,  // Wait for the new Message to arrive from the network
        Process,     // Run the Message through the database
        SinkWait,    // Wait for the database result to be sent by the network
        EndSession,  // End the session - the ServiceStateMachine will be invalid after this
        Ended        // The session has ended. It is illegal to call any method besides
                     // state() if this is the current state.
    };

    /*
     * runNext() will run the current state of the state machine. It also handles all the error
     * handling and state management for requests.
     *
     * Each state function (processMessage(), sinkCallback(), etc) should always unwind the stack
     * if they have just completed a database operation to make sure that this doesn't infinitely
     * recurse.
     */
    void runNext();

    /*
     * scheduleNext() schedules a call to runNext() in the future. This will be implemented with
     * an async TransportLayer.
     *
     * It is guaranteed to unwind the stack, and not call runNext() recursively, but is not
     * guaranteed that runNext() will run after this returns.
     */
    void scheduleNext();

    /*
     * Gets the current state of connection for testing/diagnostic purposes.
     */
    State state() const {
        return _state;
    }

    /*
     * Explicitly ends the session.
     */
    void endSession();

private:
    /*
     * This function actually calls into the database and processes a request. It's broken out
     * into its own inline function for better readability.
     */
    inline void processMessage();

    /*
     * These get called by the TransportLayer when requested network I/O has completed.
     */
    void sourceCallback(Status status);
    void sinkCallback(Status status);

    /*
     * A class that wraps up lifetime management of the _dbClient and _threadName for runNext();
     */
    class ThreadGuard;
    friend class ThreadGuard;

    const transport::SessionHandle& session() const;

    State _state{State::Source};

    ServiceEntryPoint* _sep;
    bool _sync;

    ServiceContext::UniqueClient _dbClient;
    const Client* _dbClientPtr;
    const std::string _threadName;

    bool inExhaust = false;
    bool wasCompressed = false;
    Message _inMessage;
    int64_t _counter = 0;

    AtomicWord<stdx::thread::id> _currentOwningThread;
    std::atomic_flag _isOwned = ATOMIC_FLAG_INIT;  // NOLINT
};

std::ostream& operator<<(std::ostream& stream, const ServiceStateMachine::State& state);

}  // namespace mongo
