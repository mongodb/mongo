/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

namespace mongo {

class OperationContext;

namespace transport {

class NetworkingBaton;

}  // namespace transport

/**
 * A Baton is lightweight executor, with parallel forward progress guarantees.  Rather than
 * asynchronously running tasks through one, the baton records the intent of those tasks and defers
 * waiting and execution to a later call to run().
 *
 * Note: This occurs automatically when opCtx waiting on a condition variable.
 */
class Baton : public Waitable, public std::enable_shared_from_this<Baton> {
public:
    virtual ~Baton() = default;

    /**
     * Detaches a baton from an associated opCtx.
     *
     * This invokes all callbacks currently inside the baton (from the detaching thread) with a void
     * opCtx.  Also, any calls to schedule after this point will immediately invoke their callback
     * with a null opCtx.
     */
    void detach() noexcept {
        // We make this anchor so that deleting the shared_ptr inside opCtx doesn't remove the last
        // reference to this type until we return from detach.
        const auto anchor = shared_from_this();

        detachImpl();
    }

    /**
     * Schedules a callback to run on the baton.
     *
     * The function will be invoked in one of 3 ways:
     *   1. With an opCtx inside run() (I.e. opCtx->waitForConditionOrInterrupt)
     *   2. Without an opCtx inside detach()
     *   3. Without an opCtx inside schedule() after detach()
     *
     * Note that the latter two cases are indistinguishable, so the callback should be safe to run
     * inline if passed a nullptr.  Examples of such work are logging, simple cleanup and
     * rescheduling the task on another executor.
     */
    virtual void schedule(unique_function<void(OperationContext*)> func) noexcept = 0;

    /**
     * Returns a networking view of the baton, if this baton supports networking functionality
     */
    virtual transport::NetworkingBaton* networking() noexcept {
        return nullptr;
    }

    /**
     * Marks the baton to wake up on client socket disconnect
     */
    virtual void markKillOnClientDisconnect() noexcept = 0;

private:
    virtual void detachImpl() noexcept = 0;
};

using BatonHandle = std::shared_ptr<Baton>;

}  // namespace mongo
