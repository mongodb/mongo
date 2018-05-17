/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/stdx/functional.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * Provides the minimal api for a simple out of line executor that can run non-cancellable
 * callbacks.
 *
 * Adds in a minimal amount of support for futures.
 *
 * The contract for scheduling work on an executor is that it never blocks the caller.  It doesn't
 * necessarily need to offer forward progress guarantees, but actual calls to schedule() should not
 * deadlock.
 *
 * As an explicit point of implementation: it will never invoke the passed callback from within the
 * scheduling call.
 */
class OutOfLineExecutor {
public:
    /**
     * Invokes the callback on the executor, as in schedule(), returning a future with its result.
     * That future may be ready by the time the caller returns, which means that continuations
     * chained on the returned future may be invoked on the caller of execute's stack.
     */
    template <typename Callback>
    Future<FutureContinuationResult<Callback>> execute(Callback&& cb) {
        Promise<FutureContinuationResult<Callback>> promise;
        auto future = promise.getFuture();

        schedule([ cb = std::forward<Callback>(cb), sp = promise.share() ]() mutable {
            sp.setWith(std::move(cb));
        });

        return future;
    }

    /**
     * Invokes the callback on the executor.  This never happens immediately on the caller's stack.
     */
    virtual void schedule(stdx::function<void()> func) = 0;

protected:
    ~OutOfLineExecutor() noexcept {}
};

}  // namespace mongo
