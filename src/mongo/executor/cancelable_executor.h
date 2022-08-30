/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {

/**
 * An executor supporting cancellation via a cancellation token.  Given an existing
 * OutOfLineExecutor "exec" and a cancellation token "token", you can create a CancelableExecutor
 * using CancelableExecutor::make(exec, token). This executor will use "exec" to actually execute
 * any scheduled work, but will refuse to run any work after "token" has been cancelled.
 *
 * Refusal to run work is similar to any other executor's refusal: the callback is still
 * invoked, but with a non-OK status (the kCallbackCanceledErrorStatus defined below), and
 * in a different execution context than the one maintained by the executor (see the
 * class comment in out_of_line_executor.h for more details).
 *
 * Note: in cases where the CancelableExecutor will refuse to run a callback because it has been
 * cancelled, and the backing executor refuses to do the work for some other reason (i.e. shutdown),
 * the backing executor's error-status will be preferred and passed to the callback.
 *
 * This class can be used to manage ExecutorFuture chains; here's an example:
 *      ExecutorFuture(myExec)
 *          .then([] { return doThing1(); })
 *          .then([] { return doThing2(); })
 *          .thenRunOn(CancelableExecutor::make(myExec, myToken))
 *          .then([] { return doThing3(); })
 *          .thenRunOn(myExec)
 *          .then([] { return doThing4(); })
 *          .onError([](Status s) { return doThing5(s); })
 * In this example, doThing1 and doThing2 will run on myExec; doThing3()
 * will only run if myToken is not canceled when it is ready to run;
 * doThing4() will only run if doThing3() runs and completes without error;
 * doThing5 will run if doThing3() runs and errors _or_ if myToken was cancelled
 * before doThing3() ran -- in which case the status s passed to doThing5() will be
 * kCallbackCanceledErrorStatus.
 */
class CancelableExecutor : public OutOfLineExecutor {
public:
    CancelableExecutor(ExecutorPtr exec, CancellationToken tok)
        : _exec(std::move(exec)), _source(std::move(tok)) {}

    CancelableExecutor(const CancelableExecutor&) = delete;
    CancelableExecutor& operator=(const CancelableExecutor&) = delete;
    CancelableExecutor(CancelableExecutor&&) = delete;
    CancelableExecutor& operator=(CancelableExecutor&&) = delete;

    /*
     * This is the preferred way to get a CancelableExecutor, since the ExecutorFuture interface
     * expects shared_ptrs to executors it receives in its constructor or .thenRunOn.
     */
    static std::shared_ptr<CancelableExecutor> make(ExecutorPtr exec, CancellationToken token) {
        return std::make_shared<CancelableExecutor>(std::move(exec), std::move(token));
    }
    void schedule(OutOfLineExecutor::Task func) override {
        _exec->schedule([func = std::move(func), token = _source.token()](Status s) {
            if (token.isCanceled() && s.isOK()) {
                func({ErrorCodes::CallbackCanceled, "Callback Canceled"});
            } else {
                func(s);
            }
        });
    }

private:
    ExecutorPtr _exec;
    CancellationSource _source;
};
}  // namespace mongo
