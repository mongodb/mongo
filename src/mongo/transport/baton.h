
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

#include <memory>

#include "mongo/transport/transport_layer.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

namespace mongo {

class OperationContext;

namespace transport {

class TransportLayer;
class Session;
class ReactorTimer;

/**
 * A Baton is basically a networking reactor, with limited functionality and no forward progress
 * guarantees.  Rather than asynchronously running tasks through one, the baton records the intent
 * of those tasks and defers waiting and execution to a later call to run();
 *
 * Baton's provide a mechanism to allow consumers of a transport layer to execute IO themselves,
 * rather than having this occur on another thread.  This can improve performance by minimizing
 * context switches, as well as improving the readability of stack traces by grounding async
 * execution on top of a regular client call stack.
 */
class Baton : public Waitable {
public:
    virtual ~Baton() = default;

    /**
     * Detaches a baton from an associated opCtx.
     */
    virtual void detach() = 0;

    /**
     * Executes a callback on the baton via schedule.  Returns a future which will execute on the
     * baton runner.
     */
    template <typename Callback>
    Future<FutureContinuationResult<Callback>> execute(Callback&& cb) {
        auto pf = makePromiseFuture<FutureContinuationResult<Callback>>();

        schedule([ cb = std::forward<Callback>(cb), p = std::move(pf.promise) ]() mutable {
            p.setWith(std::move(cb));
        });

        return std::move(pf.future);
    }

    /**
     * Executes a callback on the baton.
     */
    virtual void schedule(unique_function<void()> func) = 0;

    /**
     * Adds a session, returning a future which activates on read/write-ability of the session.
     */
    enum class Type {
        In,
        Out,
    };
    virtual Future<void> addSession(Session& session, Type type) = 0;

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
};

using BatonHandle = std::shared_ptr<Baton>;

}  // namespace transport
}  // namespace mongo
