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

#include "mongo/util/cancellation.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

#include <memory>
#include <utility>

namespace mongo {

class OperationContext;

namespace transport {

class NetworkingBaton;

}  // namespace transport

class Baton;

using BatonHandle = std::shared_ptr<Baton>;

/**
 * A Baton is lightweight executor, with parallel forward progress guarantees.  Rather than
 * asynchronously running tasks through one, the baton records the intent of those tasks and defers
 * waiting and execution to a later call to run().
 *
 * Note: This occurs automatically when opCtx waiting on a condition variable.
 */
class Baton : public Waitable,
              public OutOfLineExecutor,
              public std::enable_shared_from_this<Baton> {
public:
    ~Baton() override = default;

    /**
     * Detaches a baton from an associated opCtx.
     *
     * This invokes all callbacks currently inside the baton (from the detaching thread) with a void
     * opCtx.  Also, any calls to schedule after this point will immediately invoke their callback
     * with a null opCtx.
     */
    void detach() {
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
    void schedule(Task func) override = 0;

    /**
     * Returns a networking view of the baton, if this baton supports networking functionality
     */
    virtual transport::NetworkingBaton* networking() {
        return nullptr;
    }

    virtual Future<void> waitUntil(Date_t expiration, const CancellationToken& token) = 0;

    /**
     * Holder for a SubBaton, detaches on destruction
     */
    class SubBatonHolder {
        friend Baton;

    public:
        SubBatonHolder(const SubBatonHolder&) = delete;
        SubBatonHolder& operator=(const SubBatonHolder&) = delete;

        SubBatonHolder(SubBatonHolder&& other)
            : _mustDetach(other._mustDetach), _baton(std::move(other._baton)) {
            other._mustDetach = false;
        }

        SubBatonHolder& operator=(SubBatonHolder&& other) {
            if (_mustDetach) {
                _baton->detach();
            }

            _mustDetach = other._mustDetach;
            _baton = std::move(other._baton);

            other._mustDetach = false;

            return *this;
        }

        ~SubBatonHolder() {
            if (_mustDetach) {
                _baton->detach();
            }
        }

        const BatonHandle& operator*() const {
            return _baton;
        }

        const BatonHandle& operator->() const {
            return _baton;
        }

        void shutdown() {
            if (!std::exchange(_mustDetach, false)) {
                return;
            }

            _baton->detach();
        }

    private:
        explicit SubBatonHolder(const BatonHandle& baton) : _baton(baton) {}

        bool _mustDetach = true;
        BatonHandle _baton;
    };

    /**
     * Makes a sub baton for this baton.  A valid sub baton should proxy requests to the underlying
     * baton until it is detached.  After that point, all jobs within the sub baton should be failed
     * with a ShutdownInProgress status and all further work should be refused.
     *
     * NOTE: The held baton will not intercept networking() related calls.  If you intend to use the
     * baton in that mode, you should use this type with the ScopedTaskExecutor to handle
     * cancellation of async networking operations.
     */
    SubBatonHolder makeSubBaton();

private:
    virtual void detachImpl() = 0;
};

}  // namespace mongo
