// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

#include <memory>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
