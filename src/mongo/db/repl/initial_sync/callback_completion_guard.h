// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

#include <functional>
#include <mutex>

#include <boost/optional.hpp>

namespace mongo {
namespace repl {

/**
 * RAII type that stores the result of callbacks written using the executor::TaskExecutor framework.
 * Only the first result passed to setResultAndCancelRemainingWork() is saved.
 * Calls '_onCompletion' on destruction with result.
 * We use an invariant to ensure that a result has been provided by the caller at destruction.
 */
template <typename Result>
class CallbackCompletionGuard {
public:
    /**
     * Function to cancel remaining work in caller after setting '_result'.
     * This function must be called while holding a lock on the caller's mutex.
     */
    using CancelRemainingWorkFn = std::function<void(WithLock lk)>;

    /**
     * Callback function to pass result to caller at destruction.
     */
    typedef std::function<void(const Result& result)> OnCompletionFn;

    /**
     * Constructor for this completion guard.
     * 'CancelRemainingWork' is called after setting the result to cancel any outstanding
     * work in the caller. 'CancelRemainingWork' must be called while holding a lock on the
     * caller's mutex.
     * 'onCompletion' is called with the result at destruction.
     */
    CallbackCompletionGuard(const CancelRemainingWorkFn& cancelRemainingWork,
                            const OnCompletionFn& onCompletion);

    /**
     * Invokes '_onCompletion' with the result.
     * Aborts if:
     *     result is not set; or
     *     '_onCompletion' throws an exception.
     */
    ~CallbackCompletionGuard();

    /**
     * Sets result if called for the first time.
     * Cancels remaining work in caller.
     * Requires either a unique_lock or lock_guard to be passed in to ensure that we call
     * _CancelRemainingWork()) while we have a lock on the callers's mutex.
     */
    void setResultAndCancelRemainingWork(const std::lock_guard<std::mutex>& lock,
                                         const Result& result);
    void setResultAndCancelRemainingWork(const std::unique_lock<std::mutex>& lock,
                                         const Result& result);
    void setResultAndCancelRemainingWork(WithLock lock, const Result& result);

private:
    /**
     * Once we verified that we have the caller's lock, this function is called by both
     * versions of setResultAndCancelRemainingWork() to set the result and cancel any
     * remaining work in the caller.
     */
    void _setResultAndCancelRemainingWork(WithLock lk, const Result& result);

    // Called at most once after setting '_result'.
    const CancelRemainingWorkFn _cancelRemainingWork;

    // Called at destruction with '_result'.
    const OnCompletionFn _onCompletion;

    // _result is guarded by the mutex of the caller instance that owns this guard object.
    boost::optional<Result> _result;
};

template <typename Result>
CallbackCompletionGuard<Result>::CallbackCompletionGuard(
    const CancelRemainingWorkFn& cancelRemainingWork, const OnCompletionFn& onCompletion)
    : _cancelRemainingWork(cancelRemainingWork), _onCompletion(onCompletion) {}

template <typename Result>
CallbackCompletionGuard<Result>::~CallbackCompletionGuard() {
    invariant(_result);

    // _onCompletion() must be called outside the caller's lock to avoid a deadlock.
    // If '_onCompletion' throws an exception, the exception will be logged (by the terminate hook)
    // and the program will abort.
    _onCompletion(*_result);
}

template <typename Result>

void CallbackCompletionGuard<Result>::setResultAndCancelRemainingWork(
    const std::lock_guard<std::mutex>& lock, const Result& result) {
    _setResultAndCancelRemainingWork(lock, result);
}

template <typename Result>
void CallbackCompletionGuard<Result>::setResultAndCancelRemainingWork(
    const std::unique_lock<std::mutex>& lock, const Result& result) {
    _setResultAndCancelRemainingWork(lock, result);
}

template <typename Result>
void CallbackCompletionGuard<Result>::setResultAndCancelRemainingWork(WithLock lock,
                                                                      const Result& result) {
    _setResultAndCancelRemainingWork(lock, result);
}

template <typename Result>
void CallbackCompletionGuard<Result>::_setResultAndCancelRemainingWork(WithLock lk,
                                                                       const Result& result) {
    if (_result) {
        return;
    }
    _result = result;

    // This is called at most once.
    _cancelRemainingWork(lk);
}

}  // namespace repl
}  // namespace mongo
