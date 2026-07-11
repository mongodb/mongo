// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/baton.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

#include <memory>

namespace mongo {

class OperationContext;

namespace transport {

class TransportLayer;
class Session;
class ReactorTimer;

/**
 * A NetworkingBaton is basically a networking reactor, with limited functionality and parallel
 * forward progress guarantees.  Rather than asynchronously running tasks through one, the baton
 * records the intent of those tasks and defers waiting and execution to a later call to run();
 *
 * NetworkingBaton's provide a mechanism to allow consumers of a transport layer to execute IO
 * themselves, rather than having this occur on another thread.  This can improve performance by
 * minimizing context switches, as well as improving the readability of stack traces by grounding
 * async execution on top of a regular client call stack.
 */
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] NetworkingBaton : public Baton {
public:
    /**
     * Adds a session, returning a future which activates on read/write-ability of the session.
     */
    enum class Type {
        In,
        Out,
    };
    virtual Future<void> addSession(Session& session, Type type) = 0;

    using Baton::waitUntil;
    /**
     * Adds a timer, returning a future which activates after a deadline.
     */
    virtual Future<void> waitUntil(const ReactorTimer& timer, Date_t expiration) = 0;

    /**
     * Cancels waiting on a session.
     *
     * Returns true if the session was in the baton to be cancelled.
     */
    virtual bool cancelSession(Session& session) = 0;

    /**
     * Cancels waiting on a timer
     *
     * Returns true if the timer was in the baton to be cancelled.
     */
    virtual bool cancelTimer(const ReactorTimer& timer) = 0;

    /**
     * Marks the baton to wake up on client session disconnect and mark the associated operation as
     * killed.
     */
    virtual void markKillOnClientDisconnect() = 0;


    NetworkingBaton* networking() final {
        return this;
    }

    virtual bool canWait() = 0;

    virtual const TransportLayer* getTransportLayer() const = 0;
};

}  // namespace transport
}  // namespace mongo
