/**
 *    Copyright (C) 2019-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/db/baton.h"
#include "merizo/transport/transport_layer.h"
#include "merizo/util/functional.h"
#include "merizo/util/future.h"
#include "merizo/util/out_of_line_executor.h"
#include "merizo/util/time_support.h"
#include "merizo/util/waitable.h"

namespace merizo {

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
class NetworkingBaton : public Baton {
public:
    /**
     * Adds a session, returning a future which activates on read/write-ability of the session.
     */
    enum class Type {
        In,
        Out,
    };
    virtual Future<void> addSession(Session& session, Type type) noexcept = 0;

    /**
     * Adds a timer, returning a future which activates after a deadline.
     */
    virtual Future<void> waitUntil(const ReactorTimer& timer, Date_t expiration) noexcept = 0;

    /**
     * Cancels waiting on a session.
     *
     * Returns true if the session was in the baton to be cancelled.
     */
    virtual bool cancelSession(Session& session) noexcept = 0;

    /**
     * Cancels waiting on a timer
     *
     * Returns true if the timer was in the baton to be cancelled.
     */
    virtual bool cancelTimer(const ReactorTimer& timer) noexcept = 0;

    NetworkingBaton* networking() noexcept final {
        return this;
    }
};

}  // namespace transport
}  // namespace merizo
