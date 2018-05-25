/**
 *    Copyright 2018 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <type_traits>

#include "mongo/base/checked_cast.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

template <typename T>
class SharedPromise;

namespace future_details {
template <typename T>
class Promise;

template <typename T>
class Future;
template <>
class Future<void>;

// Using extern constexpr to prevent the compiler from allocating storage as a poor man's c++17
// inline constexpr variable.
// TODO delete extern in c++17 because inline is the default for constexper variables.
template <typename T>
extern constexpr bool isFuture = false;
template <typename T>
extern constexpr bool isFuture<Future<T>> = true;

// This is used to "normalize" void since it can't be used as an argument and it becomes Status
// rather than StatusWith<void>.
struct FakeVoid {};

template <typename T>
using VoidToFakeVoid = std::conditional_t<std::is_void<T>::value, FakeVoid, T>;

/**
 * This is a poor-man's implementation of c++17 std::is_invocable. We should replace it with the
 * stdlib one once we can make call() use std::invoke.
 */
template <typename Func,
          typename... Args,
          typename = typename std::result_of<Func && (Args && ...)>::type>
auto is_invocable_impl(Func&& func, Args&&... args) -> std::true_type;
auto is_invocable_impl(...) -> std::false_type;

template <typename Func, typename... Args>
struct is_invocable
    : public decltype(is_invocable_impl(std::declval<Func>(), std::declval<Args>()...)) {};


// call(func, FakeVoid) -> func(Status::OK())
// This simulates the implicit Status/T overloading you get by taking a StatusWith<T> that doesn't
// work for Status/void and Status.
// TODO replace this dispatch with constexpr if in c++17
template <typename Func>
inline auto callVoidOrStatus(Func&& func, std::true_type useStatus) {
    return func(Status::OK());
}

template <typename Func>
inline auto callVoidOrStatus(Func&& func, std::false_type useStatus) {
    return func();
}

/**
 * call() normalizes arguments to hide the FakeVoid shenanigans from users of Futures.
 * In the future it may also expand tuples to argument lists.
 */
template <typename Func, typename Arg>
inline auto call(Func&& func, Arg&& arg) {
    return func(std::forward<Arg>(arg));
}

template <typename Func>
inline auto call(Func&& func) {
    return func();
}

template <typename Func>
inline auto call(Func&& func, FakeVoid) {
    auto useStatus =
        std::integral_constant<bool, (!is_invocable<Func>() && is_invocable<Func, Status>())>();
    return callVoidOrStatus(func, useStatus);
}

template <typename Func>
inline auto call(Func&& func, StatusWith<FakeVoid> sw) {
    return func(sw.getStatus());
}

/**
 * statusCall() normalizes return values so everything returns StatusWith<T>. Exceptions are
 * converted to !OK statuses. void and Status returns are converted to StatusWith<FakeVoid>
 */
template <
    typename Func,
    typename... Args,
    typename RawResult = decltype(call(std::declval<Func>(), std::declval<Args>()...)),
    typename = std::enable_if_t<!std::is_void<RawResult>::value &&
                                !std::is_same<RawResult, Status>::value>,
    typename Result = std::conditional_t<isStatusWith<RawResult>, RawResult, StatusWith<RawResult>>>
inline Result statusCall(Func&& func, Args&&... args) noexcept {
    try {
        return call(func, std::forward<Args>(args)...);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

template <typename Func,
          typename... Args,
          typename RawResult = decltype(call(std::declval<Func>(), std::declval<Args>()...)),
          typename = std::enable_if_t<std::is_void<RawResult>::value>>
inline StatusWith<FakeVoid> statusCall(Func&& func, Args&&... args) noexcept {
    try {
        call(func, std::forward<Args>(args)...);
        return FakeVoid{};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

template <typename Func,
          typename... Args,
          typename RawResult = decltype(call(std::declval<Func>(), std::declval<Args>()...)),
          typename = std::enable_if_t<std::is_same<RawResult, Status>::value>,
          typename = void,
          typename = void>
inline StatusWith<FakeVoid> statusCall(Func&& func, Args&&... args) noexcept {
    try {
        auto status = call(func, std::forward<Args>(args)...);
        if (status.isOK())
            return FakeVoid{};
        return std::move(status);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

/**
 * throwingCall() normalizes return values so everything returns T or FakeVoid. !OK Statuses are
 * converted exceptions. void and Status returns are converted to FakeVoid.
 *
 * This is equivalent to uassertStatusOK(statusCall(func, args...)), but avoids catching just to
 * rethrow.
 */
template <
    typename Func,
    typename... Args,
    typename Result = decltype(call(std::declval<Func>(), std::declval<Args>()...)),
    typename = std::enable_if_t<!std::is_void<Result>::value && !isStatusOrStatusWith<Result>>>
inline Result throwingCall(Func&& func, Args&&... args) {
    return call(func, std::forward<Args>(args)...);
}

template <typename Func,
          typename... Args,
          typename Result = decltype(call(std::declval<Func>(), std::declval<Args>()...)),
          typename = std::enable_if_t<std::is_void<Result>::value>>
inline FakeVoid throwingCall(Func&& func, Args&&... args) {
    call(func, std::forward<Args>(args)...);
    return FakeVoid{};
}

template <typename Func,
          typename... Args,
          typename Result = decltype(call(std::declval<Func>(), std::declval<Args>()...)),
          typename = std::enable_if_t<std::is_same<Result, Status>::value>,
          typename = void>
inline FakeVoid throwingCall(Func&& func, Args&&... args) {
    uassertStatusOK(call(func, std::forward<Args>(args)...));
    return FakeVoid{};
}

template <typename Func,
          typename... Args,
          typename StatusWithResult = decltype(call(std::declval<Func>(), std::declval<Args>()...)),
          typename = std::enable_if_t<isStatusWith<StatusWithResult>>,
          typename = void,
          typename = void>
inline typename StatusWithResult::value_type throwingCall(Func&& func, Args&&... args) noexcept {
    return uassertStatusOK(call(func, std::forward<Args>(args)...));
}

template <typename Func, typename... Args>
using RawNormalizedCallResult =
    decltype(throwingCall(std::declval<Func>(), std::declval<Args>()...));

template <typename Func, typename... Args>
using NormalizedCallResult =
    std::conditional_t<std::is_same<RawNormalizedCallResult<Func, Args...>, FakeVoid>::value,
                       void,
                       RawNormalizedCallResult<Func, Args...>>;

template <typename T>
struct FutureContinuationResultImpl {
    using type = T;
};
template <typename T>
struct FutureContinuationResultImpl<Future<T>> {
    using type = T;
};
template <typename T>
struct FutureContinuationResultImpl<StatusWith<T>> {
    using type = T;
};
template <>
struct FutureContinuationResultImpl<Status> {
    using type = void;
};

/**
 * A base class that handles the ref-count for boost::intrusive_ptr compatibility.
 *
 * This is taken from RefCountable which is used for the aggregation types, adding in a way to set
 * the refcount non-atomically during initialization. Also using explicit memory orderings for all
 * operations on the count.
 * TODO look into merging back.
 */
class FutureRefCountable {
    MONGO_DISALLOW_COPYING(FutureRefCountable);

public:
    /**
     * Sets the refcount to count, assuming it is currently one less. This should only be used
     * during logical initialization before another thread could possibly have access to this
     * object.
     */
    void threadUnsafeIncRefCountTo(uint32_t count) const {
        dassert(_count.load(std::memory_order_relaxed) == (count - 1));
        _count.store(count, std::memory_order_relaxed);
    }

    friend void intrusive_ptr_add_ref(const FutureRefCountable* ptr) {
        // See this for a description of why relaxed is OK here. It is also used in libc++.
        // http://www.boost.org/doc/libs/1_66_0/doc/html/atomic/usage_examples.html#boost_atomic.usage_examples.example_reference_counters.discussion
        ptr->_count.fetch_add(1, std::memory_order_relaxed);
    };

    friend void intrusive_ptr_release(const FutureRefCountable* ptr) {
        if (ptr->_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete ptr;
        }
    };

protected:
    FutureRefCountable() = default;
    virtual ~FutureRefCountable() = default;

private:
    mutable std::atomic<uint32_t> _count{0};  // NOLINT
};

template <typename T,
          typename... Args,
          typename = std::enable_if_t<std::is_base_of<FutureRefCountable, T>::value>>
boost::intrusive_ptr<T> make_intrusive(Args&&... args) {
    auto ptr = new T(std::forward<Args>(args)...);
    ptr->threadUnsafeIncRefCountTo(1);
    return boost::intrusive_ptr<T>(ptr, /*add ref*/ false);
}


template <typename T>
struct SharedStateImpl;

template <typename T>
using SharedState = SharedStateImpl<VoidToFakeVoid<T>>;

/**
 * SSB is SharedStateBase, and this is its current state.
 */
enum class SSBState : uint8_t {
    kInit,
    kWaiting,
    kFinished,  // This should stay last since we have code like assert(state < kFinished).
};

class SharedStateBase : public FutureRefCountable {
public:
    SharedStateBase(const SharedStateBase&) = delete;
    SharedStateBase(SharedStateBase&&) = delete;
    SharedStateBase& operator=(const SharedStateBase&) = delete;
    SharedStateBase& operator=(SharedStateBase&&) = delete;

    virtual ~SharedStateBase() = default;

    // Only called by future side.
    void wait() noexcept {
        if (state.load(std::memory_order_acquire) == SSBState::kFinished)
            return;

        cv.emplace();

        auto oldState = SSBState::kInit;
        if (MONGO_unlikely(!state.compare_exchange_strong(
                oldState, SSBState::kWaiting, std::memory_order_acq_rel))) {
            // transitionToFinished() transitioned after we did our initial check.
            dassert(oldState == SSBState::kFinished);
            return;
        }

        stdx::unique_lock<stdx::mutex> lk(mx);
        cv->wait(lk, [&] {
            // The mx locking above is insufficient to establish an acquire if state transitions to
            // kFinished before we get here, but we aquire mx before the producer does.
            return state.load(std::memory_order_acquire) == SSBState::kFinished;
        });
    }

    // Remaining methods only called from promise side.
    void transitionToFinished() noexcept {
        auto oldState = state.exchange(SSBState::kFinished, std::memory_order_acq_rel);
        if (oldState == SSBState::kInit)
            return;

        dassert(oldState == SSBState::kWaiting);

        DEV {
            // If you hit this limit one of two things has probably happened
            //
            // 1. The justForContinuation optimization isn't working.
            // 2. You may be creating a variable length chain.
            //
            // If those statements don't mean anything to you, please ask an editor of this file.
            // If they don't work here anymore, I'm sorry.
            const size_t kMaxDepth = 32;

            size_t depth = 0;
            for (auto ssb = continuation.get(); ssb;
                 ssb = ssb->state.load(std::memory_order_acquire) == SSBState::kWaiting
                     ? ssb->continuation.get()
                     : nullptr) {
                depth++;

                invariant(depth < kMaxDepth);
            }
        }

        if (callback) {
            callback(this);
        }

        if (cv) {
            stdx::unique_lock<stdx::mutex> lk(mx);
            // This must be done inside the lock to correctly synchronize with wait().
            cv->notify_all();
        }
    }

    void setError(Status statusArg) noexcept {
        invariant(!statusArg.isOK());
        dassert(state.load() < SSBState::kFinished, statusArg.toString());
        status = std::move(statusArg);
        transitionToFinished();
    }

    //
    // Concurrency Rules for members: Each non-atomic member is initially owned by either the
    // Promise side or the Future side, indicated by a P/F comment. The general rule is that members
    // representing the propagating data are owned by Promise, while members representing what
    // to do with the data are owned by Future. The owner may freely modify the members it owns
    // until it releases them by doing a release-store to state of kFinished from Promise or
    // kWaiting from Future. Promise can acquire access to all members by doing an acquire-load of
    // state and seeing kWaiting (or Future with kFinished). Transitions should be done via
    // acquire-release exchanges to combine both actions.
    //
    // Future::propagateResults uses an alternative mechanism to transfer ownership of the
    // continuation member. The logical Future-side does a release-store of true to
    // isJustForContinuation, and the Promise-side can do an acquire-load seeing true to get access.
    //


    std::atomic<SSBState> state{SSBState::kInit};  // NOLINT

    // This is used to prevent infinite chains of SharedStates that just propagate results.
    std::atomic<bool> isJustForContinuation{false};  // NOLINT

    // This is likely to be a different derived type from this, since it is the logical output of
    // callback.
    boost::intrusive_ptr<SharedStateBase> continuation;  // F

    // Takes this as argument and usually writes to continuation.
    std::function<void(SharedStateBase* input)> callback;  // F


    // These are only used to signal completion to blocking waiters. Benchmarks showed that it was
    // worth deferring the construction of cv, so it can be avoided when it isn't necessary.
    stdx::mutex mx;                                // F (not that it matters)
    boost::optional<stdx::condition_variable> cv;  // F

    Status status = Status::OK();  // P

protected:
    SharedStateBase() = default;
};

template <typename T>
struct SharedStateImpl final : SharedStateBase {
    MONGO_STATIC_ASSERT(!std::is_void<T>::value);

    // Remaining methods only called by promise side.
    void fillFrom(SharedState<T>&& other) {
        dassert(state.load() < SSBState::kFinished);
        dassert(other.state.load() == SSBState::kFinished);
        if (other.status.isOK()) {
            data = std::move(other.data);
        } else {
            status = std::move(other.status);
        }
        transitionToFinished();
    }

    template <typename... Args>
    void emplaceValue(Args&&... args) noexcept {
        dassert(state.load() < SSBState::kFinished);
        try {
            data.emplace(std::forward<Args>(args)...);
        } catch (const DBException& ex) {
            status = ex.toStatus();
        }
        transitionToFinished();
    }

    void setFromStatusWith(StatusWith<T> sw) {
        if (sw.isOK()) {
            emplaceValue(std::move(sw.getValue()));
        } else {
            setError(std::move(sw.getStatus()));
        }
    }

    boost::optional<T> data;  // P
};
}  // namespace future_details

// These are in the future_details namespace to get access to its contents, but they are part of the
// public API.
using future_details::Promise;
using future_details::Future;

/**
 * This class represents the producer side of a Future.
 *
 * This is a single-shot class. You may only extract the Future once, and you may either set a value
 * or error at most once. Extracting the future and setting the value/error can be done in either
 * order.
 *
 * If the Future has been extracted, but no value or error has been set at the time this Promise is
 * destroyed, a error will be set with ErrorCode::BrokenPromise. This should generally be considered
 * a programmer error, and should not be relied upon. We may make it debug-fatal in the future.
 *
 * Only one thread can use a given Promise at a time. It is legal to have different threads setting
 * the value/error and extracting the Future, but it is the user's responsibility to ensure that
 * those calls are strictly synchronized. This is usually easiest to achieve by calling
 * makePromiseFuture<T>() then passing a SharedPromise to the completing threads.
 *
 * If the result is ready when producing the Future, it is more efficient to use
 * makeReadyFutureWith() or Future<T>::makeReady() than to use a Promise<T>.
 */
template <typename T>
class future_details::Promise {
public:
    using value_type = T;

    Promise() = default;

    ~Promise() {
        if (MONGO_unlikely(sharedState)) {
            if (haveExtractedFuture) {
                sharedState->setError({ErrorCodes::BrokenPromise, "broken promise"});
            }
        }
    }

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    // If we want to enable move-assignability, we need to handle breaking the promise on the old
    // value of this.
    Promise& operator=(Promise&&) = delete;

    // The default move construction is fine.
    Promise(Promise&&) = default;

    /**
     * Sets a value or error into this Promise by calling func, which must take no arguments and
     * return one of T, StatusWith<T> (or Status when T is void), or Future<T>. All errors, whether
     * returned or thrown, will be correctly propagated.
     *
     * If the function returns a Future<T>, this Promise's Future will complete when the returned
     * Future<T> completes, as-if it was passed to Promise::setFrom().
     *
     * If any work is needed to produce the result, prefer doing something like:
     *     promise.setWith([&]{ return makeResult(); });
     * over code like:
     *     promise.emplaceValue(makeResult());
     * because this method will correctly propagate errors thrown from makeResult(), rather than
     * ErrorCodes::BrokenPromise.
     */
    template <typename Func>
    void setWith(Func&& func) noexcept;

    /**
     * Sets the value into this Promise when the passed-in Future completes, which may have already
     * happened. If it hasn't, it is still safe to destroy this Promise since it is no longer
     * involved.
     */
    void setFrom(Future<T>&& future) noexcept;

    template <typename... Args>
    void emplaceValue(Args&&... args) noexcept {
        setImpl([&] { sharedState->emplaceValue(std::forward<Args>(args)...); });
    }

    void setError(Status status) noexcept {
        invariant(!status.isOK());
        setImpl([&] { sharedState->setError(std::move(status)); });
    }

    // TODO rename to not XXXWith and handle void
    void setFromStatusWith(StatusWith<T> sw) noexcept {
        setImpl([&] { sharedState->setFromStatusWith(std::move(sw)); });
    }

    /**
     * Get a copyable SharedPromise that can be used to complete this Promise's Future.
     *
     * Callers are required to extract the Future before calling share() to prevent race conditions.
     * Even with a SharedPromise, callers must ensure it is only completed at most once. Copyability
     * is primarily to allow capturing lambdas to be put in std::functions which don't support
     * move-only types.
     *
     * It is safe to destroy the original Promise as soon as this call returns.
     */
    SharedPromise<T> share() noexcept;

    /**
     * Prefer using makePromiseFuture<T>() over constructing a promise and calling this method.
     */
    Future<T> getFuture() noexcept;

private:
    friend class Future<void>;

    template <typename Func>
    void setImpl(Func&& doSet) noexcept {
        invariant(!haveSetValue);
        haveSetValue = true;
        doSet();
        if (haveExtractedFuture)
            sharedState.reset();
    }

    bool haveSetValue = false;
    bool haveExtractedFuture = false;
    boost::intrusive_ptr<SharedState<T>> sharedState = make_intrusive<SharedState<T>>();
};

/**
 * A SharedPromise is a copyable object that can be used to complete a Promise.
 *
 * All copies derived from the same call to Promise::share() will complete the same shared state.
 * Callers must ensure that the shared state is only completed at most once. Copyability is
 * primarily to allow capturing lambdas to be put in std::functions which don't support move-only
 * types. If the final derived SharedPromise is destroyed without completion, the Promise will be
 * broken.
 *
 * All methods behave the same as on the underlying Promise.
 */
template <typename T>
class SharedPromise {
public:
    SharedPromise() = default;

    template <typename Func>
    void setWith(Func&& func) noexcept {
        _promise->setWith(std::forward<Func>(func));
    }

    void setFrom(Future<T>&& future) noexcept {
        _promise->setFrom(std::move(future));
    }

    template <typename... Args>
    void emplaceValue(Args&&... args) noexcept {
        _promise->emplaceValue(std::forward<Args>(args)...);
    }

    void setError(Status status) noexcept {
        _promise->setError(std::move(status));
    }

private:
    // Only Promise<T> needs to be a friend, but MSVC2015 doesn't respect that friendship.
    // TODO see if this is still needed on MSVC2017+
    template <typename T2>
    friend class Promise;

    explicit SharedPromise(std::shared_ptr<Promise<T>>&& promise) : _promise(std::move(promise)) {}

    // TODO consider adding a SharedPromise refcount to SharedStateBase to avoid the extra
    // allocation. The tricky part will be ensuring that BrokenPromise is set when the last copy is
    // destroyed.
    std::shared_ptr<Promise<T>> _promise;
};

/**
 * Future<T> is logically a possibly-deferred StatusWith<T> (or Status when T is void).
 *
 * As is usual for rvalue-qualified methods, you may call at most one of them on a given Future.
 *
 * A future may be passed between threads, but only one thread may use it at a time.
 *
 * TODO decide if destroying a Future before extracting the result should cancel work or should
 * cancellation be explicit. For now avoid unnecessarily throwing away active Futures since the
 * behavior may change. End all Future chains with either a blocking call to get()/getNoThrow() or a
 * non-blocking call to getAsync().
 */
template <typename T>
class MONGO_WARN_UNUSED_RESULT_CLASS future_details::Future {
public:
    static_assert(!std::is_same<T, Status>::value,
                  "Future<Status> is banned. Use Future<void> instead.");
    static_assert(!isStatusWith<T>, "Future<StatusWith<T>> is banned. Just use Future<T> instead.");
    static_assert(!isFuture<T>, "Future<Future<T>> is banned. Just use Future<T> instead.");
    static_assert(!std::is_reference<T>::value, "Future<T&> is banned.");
    static_assert(!std::is_const<T>::value, "Future<const T> is banned.");
    static_assert(!std::is_array<T>::value, "Future<T[]> is banned.");

    using value_type = T;

    /**
     * Constructs a Future in a moved-from state that can only be assigned to or destroyed.
     */
    Future() = default;

    Future& operator=(Future&&) = default;
    Future(Future&&) = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    /* implicit */ Future(T val) : Future(makeReady(std::move(val))) {}
    /* implicit */ Future(Status status) : Future(makeReady(std::move(status))) {}
    /* implicit */ Future(StatusWith<T> sw) : Future(makeReady(std::move(sw))) {}

    /**
     * Make a ready Future<T> from a value for cases where you don't need to wait asynchronously.
     *
     * Calling this is faster than getting a Future out of a Promise, and is effectively free. It is
     * fast enough that you never need to avoid returning a Future from an API, even if the result
     * is ready 99.99% of the time.
     *
     * As an example, if you are handing out results from a batch, you can use this when for each
     * result while you have a batch, then use a Promise to return a not-ready Future when you need
     * to get another batch.
     */
    static Future<T> makeReady(T val) {  // TODO emplace?
        Future out;
        out.immediate = std::move(val);
        return out;
    }

    static Future<T> makeReady(Status status) {
        invariant(!status.isOK());
        auto out = Future<T>(make_intrusive<SharedState<T>>());
        out.shared->setError(std::move(status));
        return out;
    }

    static Future<T> makeReady(StatusWith<T> val) {
        if (val.isOK())
            return makeReady(std::move(val.getValue()));
        return makeReady(val.getStatus());
    }

    /**
     * If this returns true, get() is guaranteed not to block and callbacks will be immediately
     * invoked. You can't assume anything if this returns false since it may be completed
     * immediately after checking (unless you have independent knowledge that this Future can't
     * complete in the background).
     *
     * Callers must still call get() or similar, even on Future<void>, to ensure that they are
     * correctly sequenced with the completing task, and to be informed about whether the Promise
     * completed successfully.
     *
     * This is generally only useful as an optimization to avoid prep work, such as setting up
     * timeouts, that is unnecessary if the Future is ready already.
     */
    bool isReady() const {
        // This can be a relaxed load because callers are not allowed to use it to establish
        // ordering.
        return immediate || shared->state.load(std::memory_order_relaxed) == SSBState::kFinished;
    }

    /**
     * Gets the value out of this Future, blocking until it is ready.
     *
     * get() methods throw on error, while getNoThrow() returns a !OK status.
     *
     * These methods can be called multiple times, except for the rvalue overloads.
     */
    T get() && {
        return std::move(getImpl());
    }
    T& get() & {
        return getImpl();
    }
    const T& get() const& {
        return const_cast<Future*>(this)->getImpl();
    }
    StatusWith<T> getNoThrow() && noexcept {
        if (immediate) {
            return std::move(*immediate);
        }

        shared->wait();
        if (!shared->status.isOK())
            return std::move(shared->status);
        return std::move(*shared->data);
    }
    StatusWith<T> getNoThrow() const& noexcept {
        if (immediate) {
            return *immediate;
        }

        shared->wait();
        if (!shared->status.isOK())
            return shared->status;
        return *shared->data;
    }

    /**
     * This ends the Future continuation chain by calling a callback on completion. Use this to
     * escape back into a callback-based API.
     *
     * For now, the callback must not fail, since there is nowhere to propagate the error to.
     * TODO decide how to handle func throwing.
     */
    template <typename Func>  // StatusWith<T> -> void
        void getAsync(Func&& func) && noexcept {
        static_assert(std::is_void<decltype(call(func, std::declval<StatusWith<T>>()))>::value,
                      "func passed to getAsync must return void");

        return generalImpl(
            // on ready success:
            [&](T&& val) { call(func, std::move(val)); },
            // on ready failure:
            [&](Status&& status) { call(func, std::move(status)); },
            // on not ready yet:
            [&] {
                shared->callback = [func = std::forward<Func>(func)](SharedStateBase *
                                                                     ssb) mutable noexcept {
                    const auto input = checked_cast<SharedState<T>*>(ssb);
                    if (input->status.isOK()) {
                        call(func, std::move(*input->data));
                    } else {
                        call(func, std::move(input->status));
                    }
                };
            });
    }

    //
    // The remaining methods are all continuation based and take a callback and return a Future.
    // Each method has a comment indicating the supported signatures for that callback, and a
    // description of when the callback is invoked and how the impacts the returned Future. It may
    // be helpful to think of Future continuation chains as a pipeline of stages that take input
    // from earlier stages and produce output for later stages.
    //
    // Be aware that the callback may be invoked inline at the call-site or at the producer when
    // setting the value. Therefore, you should avoid doing blocking work inside of a callback.
    // Additionally, avoid acquiring any locks or mutexes that the caller already holds, otherwise
    // you risk a deadlock. If either of these concerns apply to your callback, it should schedule
    // itself on an executor, rather than doing work in the callback.
    // TODO make this easier to do by having executor APIs return Futures.
    //
    // Error handling in callbacks: all exceptions thrown propagate to the returned Future
    // automatically. Callbacks that return Status or StatusWith<T> behave as-if they were wrapped
    // in something that called uassertStatusOK() on the return value. There is no way to
    // distinguish between a function throwing or returning a !OK status.
    //
    // Callbacks that return Future<T> are automatically unwrapped and connected to the returned
    // Future<T>, rather than producing a Future<Future<T>>.
    //

    /**
     * Callbacks passed to then() are only called if the input Future completes successfully.
     * Otherwise the error propagates automatically, bypassing the callback.
     */
    template <typename Func,  // T -> Result or T -> StatusWith<Result>
              typename Result = NormalizedCallResult<Func, T>,
              typename = std::enable_if_t<!isFuture<Result>>>
        Future<Result> then(Func&& func) && noexcept {
        return generalImpl(
            // on ready success:
            [&](T&& val) { return Future<Result>::makeReady(statusCall(func, std::move(val))); },
            // on ready failure:
            [&](Status&& status) { return Future<Result>::makeReady(std::move(status)); },
            // on not ready yet:
            [&] {
                return makeContinuation<Result>([func = std::forward<Func>(func)](
                    SharedState<T> * input, SharedState<Result> * output) mutable noexcept {
                    if (!input->status.isOK())
                        return output->setError(std::move(input->status));

                    output->setFromStatusWith(statusCall(func, std::move(*input->data)));
                });
            });
    }

    /**
     * Same as above then() but for case where func returns a Future that needs to be unwrapped.
     */
    template <typename Func,  // T -> Future<UnwrappedResult>
              typename RawResult = NormalizedCallResult<Func, T>,
              typename = std::enable_if_t<isFuture<RawResult>>,
              typename UnwrappedResult = typename RawResult::value_type>
        Future<UnwrappedResult> then(Func&& func) && noexcept {
        return generalImpl(
            // on ready success:
            [&](T&& val) {
                try {
                    return Future<UnwrappedResult>(throwingCall(func, std::move(val)));
                } catch (const DBException& ex) {
                    return Future<UnwrappedResult>::makeReady(ex.toStatus());
                }
            },
            // on ready failure:
            [&](Status&& status) { return Future<UnwrappedResult>::makeReady(std::move(status)); },
            // on not ready yet:
            [&] {
                return makeContinuation<UnwrappedResult>([func = std::forward<Func>(func)](
                    SharedState<T> * input,
                    SharedState<UnwrappedResult> * output) mutable noexcept {
                    if (!input->status.isOK())
                        return output->setError(std::move(input->status));

                    try {
                        throwingCall(func, std::move(*input->data)).propagateResultTo(output);
                    } catch (const DBException& ex) {
                        output->setError(ex.toStatus());
                    }
                });
            });
    }

    /**
     * Callbacks passed to onError() are only called if the input Future completes with an error.
     * Otherwise, the successful result propagates automatically, bypassing the callback.
     *
     * The callback can either produce a replacement value (which must be a T), return a replacement
     * Future<T> (such as a by retrying), or return/throw a replacement error.
     *
     * Note that this will only catch errors produced by earlier stages; it is not registering a
     * general error handler for the entire chain.
     */
    template <typename Func,  // Status -> T or Status -> StatusWith<T>
              typename Result = RawNormalizedCallResult<Func, Status>,
              typename = std::enable_if_t<!isFuture<Result>>>
        Future<T> onError(Func&& func) && noexcept {
        static_assert(
            std::is_same<Result, T>::value,
            "func passed to Future<T>::onError must return T, StatusWith<T>, or Future<T>");

        return generalImpl(
            // on ready success:
            [&](T&& val) { return Future<T>::makeReady(std::move(val)); },
            // on ready failure:
            [&](Status&& status) {
                return Future<T>::makeReady(statusCall(func, std::move(status)));
            },
            // on not ready yet:
            [&] {
                return makeContinuation<T>([func = std::forward<Func>(func)](
                    SharedState<T> * input, SharedState<T> * output) mutable noexcept {
                    if (input->status.isOK())
                        return output->emplaceValue(std::move(*input->data));

                    output->setFromStatusWith(statusCall(func, std::move(input->status)));
                });
            });
    }

    /**
     * Same as above onError() but for case where func returns a Future that needs to be unwrapped.
     */
    template <typename Func,  // Status -> Future<T>
              typename Result = RawNormalizedCallResult<Func, Status>,
              typename = std::enable_if_t<isFuture<Result>>,
              typename = void>
        Future<T> onError(Func&& func) && noexcept {
        static_assert(
            std::is_same<Result, Future<T>>::value ||
                (std::is_same<T, FakeVoid>::value && std::is_same<Result, Future<void>>::value),
            "func passed to Future<T>::onError must return T, StatusWith<T>, or Future<T>");

        return generalImpl(
            // on ready success:
            [&](T&& val) { return Future<T>::makeReady(std::move(val)); },
            // on ready failure:
            [&](Status&& status) {
                try {
                    return Future<T>(throwingCall(func, std::move(status)));
                } catch (const DBException& ex) {
                    return Future<T>::makeReady(ex.toStatus());
                }
            },
            // on not ready yet:
            [&] {
                return makeContinuation<T>([func = std::forward<Func>(func)](
                    SharedState<T> * input, SharedState<T> * output) mutable noexcept {
                    if (input->status.isOK())
                        return output->emplaceValue(std::move(*input->data));

                    try {
                        throwingCall(func, std::move(input->status)).propagateResultTo(output);
                    } catch (const DBException& ex) {
                        output->setError(ex.toStatus());
                    }
                });
            });
    }

    /**
     * Same as the other two onErrors but only calls the callback if the code matches the template
     * parameter. Otherwise lets the error propagate unchanged.
     */
    template <ErrorCodes::Error code, typename Func>
        Future<T> onError(Func&& func) && noexcept {
        using Result = RawNormalizedCallResult<Func, Status>;
        static_assert(
            std::is_same<Result, T>::value || std::is_same<Result, Future<T>>::value ||
                (std::is_same<T, FakeVoid>::value && std::is_same<Result, Future<void>>::value),
            "func passed to Future<T>::onError must return T, StatusWith<T>, or Future<T>");

        if (immediate || (isReady() && shared->status.isOK()))
            return std::move(*this);  // Avoid copy/moving func if we know we won't call it.

        // TODO in C++17 with constexpr if this can be done cleaner and more efficiently by not
        // throwing.
        return std::move(*this).onError([func =
                                             std::forward<Func>(func)](Status && status) mutable {
            if (status != code)
                uassertStatusOK(status);
            return throwingCall(func, std::move(status));
        });
    }

    /**
     * TODO do we need a version of then/onError like onCompletion() that handles both success and
     * Failure, but doesn't end the chain like getAsync()? Right now we don't, and we can add one if
     * we do.
     */

    //
    // The tap/tapError/tapAll family of functions take callbacks to observe the flow through a
    // future chain without affecting the propagating result, except possibly if they throw. If the
    // naming seems odd, you can think of it like a "wire tap" in that it allows you to observe a
    // conversation between two parties (the promise-producer and future-consumer) without adding
    // messages of your own. This is why all callbacks are required to return void.
    //
    // TODO decide what to do if callback throws:
    //  - transition the future chain to failure
    //  - ignore
    //  - fatal (current impl)
    //

    /**
     * Callback is called if the input completes successfully.
     *
     * This can be used to inform some outside system of the result.
     */
    template <typename Func>  // T -> void
        Future<T> tap(Func&& func) && noexcept {
        static_assert(std::is_void<decltype(call(func, std::declval<const T&>()))>::value,
                      "func passed to tap must return void");

        return tapImpl(std::forward<Func>(func),
                       [](Func && func, const T& val) noexcept { call(func, val); },
                       [](Func && func, const Status& status) noexcept {});
    }

    /**
     * Callback is called if the input completes with an error.
     *
     * This can be used to log.
     */
    template <typename Func>  // Status -> void
        Future<T> tapError(Func&& func) && noexcept {
        static_assert(std::is_void<decltype(call(func, std::declval<const Status&>()))>::value,
                      "func passed to tapError must return void");

        return tapImpl(std::forward<Func>(func),
                       [](Func && func, const T& val) noexcept {},
                       [](Func && func, const Status& status) noexcept { call(func, status); });
    }

    /**
     * Callback is called when the input completes, regardless of success or failure.
     *
     * This can be used for cleanup. Some other libraries name the equivalent method finally to
     * match the common semantic from other languages.
     *
     * Warning: If func takes a StatusWith<T>, it requires copying the value on success. If that is
     * too expensive, it can be avoided by either providing a function object with separate
     * Status/const T& overloads, or by using a generic lambda if you don't need to consult the
     * value for your cleanup.
     */
    template <typename Func>  // StatusWith<T> -> void, or Status/const T& overloads.
        Future<T> tapAll(Func&& func) && noexcept {
        static_assert(std::is_void<decltype(call(func, std::declval<const T&>()))>::value,
                      "func passed to tapAll must return void");
        static_assert(std::is_void<decltype(call(func, std::declval<const Status&>()))>::value,
                      "func passed to tapAll must return void");

        return tapImpl(std::forward<Func>(func),
                       [](Func && func, const T& val) noexcept { call(func, val); },
                       [](Func && func, const Status& status) noexcept { call(func, status); });
    }

    /**
     * Ignores the return value of a future, transforming it down into a Future<void>.
     *
     * This only ignores values, not errors.  Those remain propogated until an onError handler.
     *
     * Equivalent to then([](auto&&){});
     */
    Future<void> ignoreValue() && noexcept;

private:
    template <typename T2>
    friend class Future;
    friend class Promise<T>;

    T& getImpl() {
        if (immediate) {
            return *immediate;
        }

        shared->wait();
        uassertStatusOK(shared->status);
        return *(shared->data);
    }

    // All callbacks are called immediately so they are allowed to capture everything by reference.
    // All callbacks should return the same return type.
    template <typename SuccessFunc, typename FailFunc, typename NotReady>
    auto generalImpl(SuccessFunc&& success, FailFunc&& fail, NotReady&& notReady) noexcept {
        if (immediate) {
            return success(std::move(*immediate));
        }

        if (shared->state.load(std::memory_order_acquire) == SSBState::kFinished) {
            if (shared->status.isOK()) {
                return success(std::move(*shared->data));
            } else {
                return fail(std::move(shared->status));
            }
        }

        // This is always done after notReady, which never throws. It is in an ON_BLOCK_EXIT to
        // support both void- and value-returning notReady implementations since we can't assign
        // void to a variable.
        ON_BLOCK_EXIT([&] {
            auto oldState = SSBState::kInit;
            if (MONGO_unlikely(!shared->state.compare_exchange_strong(
                    oldState, SSBState::kWaiting, std::memory_order_acq_rel))) {
                dassert(oldState == SSBState::kFinished);
                shared->callback(shared.get());
            }
        });

        return notReady();
    }

    // success and fail may be called from a continuation so they shouldn't capture anything.
    template <typename Callback, typename SuccessFunc, typename FailFunc>
    Future<T> tapImpl(Callback&& cb, SuccessFunc&& success, FailFunc&& fail) noexcept {
        // Make sure they don't capture anything.
        MONGO_STATIC_ASSERT(std::is_empty<SuccessFunc>::value);
        MONGO_STATIC_ASSERT(std::is_empty<FailFunc>::value);

        return generalImpl(
            [&](T&& val) {
                success(std::forward<Callback>(cb), stdx::as_const(val));
                return Future<T>::makeReady(std::move(val));
            },
            [&](Status&& status) {
                fail(std::forward<Callback>(cb), stdx::as_const(status));
                return Future<T>::makeReady(std::move(status));
            },
            [&] {
                return makeContinuation<T>([ success, fail, cb = std::forward<Callback>(cb) ](
                    SharedState<T> * input, SharedState<T> * output) mutable noexcept {
                    if (input->status.isOK()) {
                        success(std::forward<Callback>(cb), stdx::as_const(*input->data));
                    } else {
                        fail(std::forward<Callback>(cb), stdx::as_const(input->status));
                    }

                    output->fillFrom(std::move(*input));
                });
            });
    }

    void propagateResultTo(SharedState<T>* output) noexcept {
        generalImpl(
            // on ready success:
            [&](T&& val) { output->emplaceValue(std::move(val)); },
            // on ready failure:
            [&](Status&& status) { output->setError(std::move(status)); },
            // on not ready yet:
            [&] {
                // If the output is just for continuation, bypass it and just directly fill in the
                // SharedState that it would write to. The concurrency situation is a bit subtle
                // here since we are the Future-side of shared, but the Promise-side of output.
                // The rule is that p->isJustForContinuation must be acquire-read as true before
                // examining p->continuation, and p->continuation must be written before doing the
                // release-store of true to p->isJustForContinuation.
                if (output->isJustForContinuation.load(std::memory_order_acquire)) {
                    shared->continuation = std::move(output->continuation);
                } else {
                    shared->continuation = output;
                }
                shared->isJustForContinuation.store(true, std::memory_order_release);

                shared->callback = [](SharedStateBase * ssb) noexcept {
                    const auto input = checked_cast<SharedState<T>*>(ssb);
                    const auto output = checked_cast<SharedState<T>*>(ssb->continuation.get());
                    output->fillFrom(std::move(*input));
                };
            });
    }

    template <typename Result, typename OnReady>
    inline Future<Result> makeContinuation(OnReady&& onReady) {
        invariant(!shared->callback && !shared->continuation);

        auto continuation = make_intrusive<SharedState<Result>>();
        continuation->threadUnsafeIncRefCountTo(2);
        shared->continuation.reset(continuation.get(), /*add ref*/ false);
        shared->callback = [onReady = std::forward<OnReady>(onReady)](SharedStateBase *
                                                                      ssb) mutable noexcept {
            const auto input = checked_cast<SharedState<T>*>(ssb);
            const auto output = checked_cast<SharedState<Result>*>(ssb->continuation.get());
            onReady(input, output);
        };
        return Future<VoidToFakeVoid<Result>>(std::move(continuation));
    }

    explicit Future(boost::intrusive_ptr<SharedState<T>> ptr) : shared(std::move(ptr)) {}

    // At most one of these will be active.
    boost::optional<T> immediate;
    boost::intrusive_ptr<SharedState<T>> shared;
};

/**
 * The void specialization of Future<T>. See the general Future<T> for detailed documentation.
 * It should be the same as the generic Future<T> with the following exceptions:
 *   - Anything mentioning StatusWith<T> will use Status instead.
 *   - Anything returning references to T will just return void since there are no void references.
 *   - Anything taking a T argument will receive no arguments.
 */
template <>
class MONGO_WARN_UNUSED_RESULT_CLASS future_details::Future<void> {
public:
    using value_type = void;

    /* implicit */ Future() : Future(makeReady()) {}
    /* implicit */ Future(Status status) : Future(makeReady(std::move(status))) {}

    static Future<void> makeReady() {
        return Future<FakeVoid>::makeReady(FakeVoid{});
    }

    static Future<void> makeReady(Status status) {
        if (status.isOK())
            return makeReady();
        return Future<FakeVoid>::makeReady(std::move(status));
    }

    bool isReady() const {
        return inner.isReady();
    }

    void get() const {
        inner.get();
    }

    Status getNoThrow() const noexcept {
        return inner.getNoThrow().getStatus();
    }

    template <typename Func>  // Status -> void
        void getAsync(Func&& func) && noexcept {
        return std::move(inner).getAsync(std::forward<Func>(func));
    }

    template <typename Func>  // () -> T or StatusWith<T> or Future<T>
        auto then(Func&& func) && noexcept {
        return std::move(inner).then(std::forward<Func>(func));
    }

    template <typename Func>  // Status -> T or StatusWith<T> or Future<T>
        Future<void> onError(Func&& func) && noexcept {
        return std::move(inner).onError(std::forward<Func>(func));
    }

    template <ErrorCodes::Error code, typename Func>  // Status -> T or StatusWith<T> or Future<T>
        Future<void> onError(Func&& func) && noexcept {
        return std::move(inner).onError<code>(std::forward<Func>(func));
    }

    template <typename Func>  // () -> void
        Future<void> tap(Func&& func) && noexcept {
        return std::move(inner).tap(std::forward<Func>(func));
    }

    template <typename Func>  // Status -> void
        Future<void> tapError(Func&& func) && noexcept {
        return std::move(inner).tapError(std::forward<Func>(func));
    }

    template <typename Func>  // Status -> void
        Future<void> tapAll(Func&& func) && noexcept {
        return std::move(inner).tapAll(std::forward<Func>(func));
    }

    Future<void> ignoreValue() && noexcept {
        return std::move(*this);
    }

private:
    template <typename T>
    friend class Future;
    friend class Promise<void>;

    explicit Future(boost::intrusive_ptr<SharedState<FakeVoid>> ptr) : inner(std::move(ptr)) {}
    /*implicit*/ Future(Future<FakeVoid>&& inner) : inner(std::move(inner)) {}
    /*implicit*/ operator Future<FakeVoid>() && {
        return std::move(inner);
    }

    void propagateResultTo(SharedState<void>* output) noexcept {
        inner.propagateResultTo(output);
    }

    static Future<void> makeReady(StatusWith<FakeVoid> status) {
        return Future<FakeVoid>::makeReady(std::move(status));
    }

    Future<FakeVoid> inner;
};

/**
 * Makes a ready Future with the return value of a nullary function. This has the same semantics as
 * Promise::setWith, and has the same reasons to prefer it over Future<T>::makeReady(). Also, it
 * deduces the T, so it is easier to use.
 */
template <typename Func>
auto makeReadyFutureWith(Func&& func) {
    return Future<void>::makeReady().then(std::forward<Func>(func));
}

/**
 * Returns a bound Promise and Future in a struct with friendly names (promise and future) that also
 * works well with C++17 structured bindings.
 */
template <typename T>
inline auto makePromiseFuture() {
    struct PromiseAndFuture {
        Promise<T> promise;
        Future<T> future = promise.getFuture();
    };
    return PromiseAndFuture();
}

/**
 * This metafunction allows APIs that take callbacks and return Future to avoid doing their own type
 * calculus. This results in the base value_type that would result from passing Func to a
 * Future<T>::then(), with the same normalizing of T/StatusWith<T>/Future<T> returns. This is
 * primarily useful for implementations of executors rather than their users.
 *
 * This returns the unwrapped T rather than Future<T> so it will be easy to create a Promise<T>.
 *
 * Examples:
 *
 * FutureContinuationResult<std::function<void()>> == void
 * FutureContinuationResult<std::function<Status()>> == void
 * FutureContinuationResult<std::function<Future<void>()>> == void
 *
 * FutureContinuationResult<std::function<int()>> == int
 * FutureContinuationResult<std::function<StatusWith<int>()>> == int
 * FutureContinuationResult<std::function<Future<int>()>> == int
 *
 * FutureContinuationResult<std::function<int(bool)>, bool> == int
 *
 * FutureContinuationResult<std::function<int(bool)>, NotBool> SFINAE-safe substitution failure.
 */
template <typename Func, typename... Args>
using FutureContinuationResult =
    typename future_details::FutureContinuationResultImpl<std::result_of_t<Func(Args&&...)>>::type;

//
// Implementations of methods that couldn't be defined in the class due to ordering requirements.
//

template <typename T>
inline Future<T> Promise<T>::getFuture() noexcept {
    invariant(!haveExtractedFuture);
    haveExtractedFuture = true;

    if (!haveSetValue) {
        sharedState->threadUnsafeIncRefCountTo(2);
        return Future<T>(
            boost::intrusive_ptr<SharedState<T>>(sharedState.get(), /*add ref*/ false));
    }

    // Let the Future steal our ref-count since we don't need it anymore.
    return Future<T>(std::move(sharedState));
}

template <typename T>
inline SharedPromise<T> Promise<T>::share() noexcept {
    invariant(haveExtractedFuture);
    invariant(!haveSetValue);
    return SharedPromise<T>(std::make_shared<Promise<T>>(std::move(*this)));
}

template <typename T>
inline void Promise<T>::setFrom(Future<T>&& future) noexcept {
    setImpl([&] { future.propagateResultTo(sharedState.get()); });
}

template <typename T>
template <typename Func>
inline void Promise<T>::setWith(Func&& func) noexcept {
    setFrom(Future<void>::makeReady().then(std::forward<Func>(func)));
}

template <typename T>
    Future<void> Future<T>::ignoreValue() && noexcept {
    return std::move(*this).then([](auto&&) {});
}

}  // namespace mongo
