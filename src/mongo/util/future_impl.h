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

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <type_traits>

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

template <typename T>
class Promise;

template <typename T>
class Future;

template <typename T>
class SharedPromise;

template <typename T>
class SharedSemiFuture;

namespace future_details {
template <typename T>
class FutureImpl;
template <>
class FutureImpl<void>;

// Using extern constexpr to prevent the compiler from allocating storage as a poor man's c++17
// inline constexpr variable.
// TODO delete extern in c++17 because inline is the default for constexper variables.
template <typename T>
extern constexpr bool isFuture = false;
template <typename T>
extern constexpr bool isFuture<Future<T>> = true;

template <typename T>
extern constexpr bool isFutureLike = false;
template <typename T>
extern constexpr bool isFutureLike<Future<T>> = true;
template <typename T>
extern constexpr bool isFutureLike<SharedSemiFuture<T>> = true;

template <typename T>
struct UnwrappedTypeImpl {
    static_assert(!isFuture<T>);
    static_assert(!isStatusOrStatusWith<T>);
    using type = T;
};
template <typename T>
using UnwrappedType = typename UnwrappedTypeImpl<T>::type;
template <typename T>
struct UnwrappedTypeImpl<Future<T>> {
    using type = T;
};
template <typename T>
struct UnwrappedTypeImpl<FutureImpl<T>> {
    using type = T;
};
template <typename T>
struct UnwrappedTypeImpl<StatusWith<T>> {
    using type = T;
};
template <>
struct UnwrappedTypeImpl<Status> {
    using type = void;
};

template <typename T>
struct AddRefUnlessVoidImpl {
    using type = T&;
};
template <>
struct AddRefUnlessVoidImpl<void> {
    using type = void;
};
template <>
struct AddRefUnlessVoidImpl<const void> {
    using type = void;
};
template <typename T>
using AddRefUnlessVoid = typename AddRefUnlessVoidImpl<T>::type;

// This is used to "normalize" void since it can't be used as an argument and it becomes Status
// rather than StatusWith<void>.
struct FakeVoid {};

template <typename T>
using VoidToFakeVoid = std::conditional_t<std::is_void_v<T>, FakeVoid, T>;
template <typename T>
using FakeVoidToVoid = std::conditional_t<std::is_same_v<T, FakeVoid>, void, T>;

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
    auto useStatus = std::integral_constant<bool,
                                            (!stdx::is_invocable<Func>() &&
                                             stdx::is_invocable<Func, Status>())>();
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

template <typename T>
struct SharedStateImpl;

template <typename T>
using SharedState = SharedStateImpl<VoidToFakeVoid<T>>;

/**
 * SSB is SharedStateBase, and this is its current state.
 *
 * Legal transitions on future side:
 *      kInit -> kWaiting
 *      kInit -> kHaveContinuation
 *      kWaiting -> kHaveContinuation
 *
 * Legal transitions on promise side:
 *      kInit -> kFinished
 *      kWaiting -> kFinished
 *      kHaveContinuation -> kFinished
 *
 * Note that all and only downward transitions are legal.
 *
 * Each thread must change the state *after* it is set up all data that it is releasing to the other
 * side. This must be done with an exchange() or compareExchange() so that you know what to do if
 * the other side finished its transition before you.
 */
enum class SSBState : uint8_t {
    // Initial state: Promise hasn't been completed and has nothing to do when it is.
    kInit,

    // Promise hasn't been completed. Someone has constructed the condvar and may be waiting on it.
    // We do not transition back to kInit if they give up on waiting. There is also no continuation
    // registered in this state.
    kWaiting,

    // Promise hasn't been completed. Someone has registered a callback to be run when it is.
    //
    // There is no-one currently waiting on the condvar. TODO This assumption will need to change
    // when we add continuation support to SharedSemiFuture.
    kHaveContinuation,

    // The promise has been completed with a value or error. This is the terminal state. This should
    // stay last since we have code like assert(state < kFinished).
    kFinished,
};

class SharedStateBase : public RefCountable {
public:
    SharedStateBase(const SharedStateBase&) = delete;
    SharedStateBase(SharedStateBase&&) = delete;
    SharedStateBase& operator=(const SharedStateBase&) = delete;
    SharedStateBase& operator=(SharedStateBase&&) = delete;

    virtual ~SharedStateBase() = default;

    // Only called by future side, but may be called multiple times if waiting times out and is
    // retried.
    void wait(Interruptible* interruptible) {
        if (state.load(std::memory_order_acquire) == SSBState::kFinished)
            return;

        stdx::unique_lock<stdx::mutex> lk(mx);
        if (!cv) {
            cv.emplace();

            auto oldState = SSBState::kInit;
            if (MONGO_unlikely(!state.compare_exchange_strong(
                    oldState, SSBState::kWaiting, std::memory_order_acq_rel))) {
                // transitionToFinished() transitioned after we did our initial check.
                dassert(oldState == SSBState::kFinished);
                return;
            }
        } else {
            // Someone has already created the cv and put us in the waiting state. The promise may
            // also have completed after we checked above, so we can't assume we aren't at
            // kFinished.
            dassert(state.load() != SSBState::kInit);
        }

        interruptible->waitForConditionOrInterrupt(*cv, lk, [&] {
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

        dassert(oldState == SSBState::kWaiting || oldState == SSBState::kHaveContinuation);

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
                 ssb = ssb->state.load(std::memory_order_acquire) == SSBState::kHaveContinuation
                     ? ssb->continuation.get()
                     : nullptr) {
                depth++;

                invariant(depth < kMaxDepth);
            }
        }

        if (oldState == SSBState::kHaveContinuation) {
            invariant(callback);
            callback(this);
        } else if (cv) {
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
    unique_function<void(SharedStateBase* input)> callback;  // F


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

template <typename T>
class SharedStateHolder {
public:
    SharedStateHolder() = default;
    explicit SharedStateHolder(const boost::intrusive_ptr<SharedState<T>>& shared)
        : _shared(shared) {}
    explicit SharedStateHolder(boost::intrusive_ptr<SharedState<T>>&& shared)
        : _shared(std::move(shared)) {}

    static SharedStateHolder makeReady(T&& val) {
        auto out = SharedStateHolder(make_intrusive<SharedState<T>>());
        out._shared->emplaceValue(std::move(val));
        return out;
    }

    static SharedStateHolder makeReady(Status&& status) {
        invariant(!status.isOK());
        auto out = SharedStateHolder(make_intrusive<SharedState<T>>());
        out._shared->setError(std::move(status));
        return out;
    }

    static SharedStateHolder makeReady(StatusWith<T>&& val) {
        if (val.isOK())
            return makeReady(std::move(val.getValue()));
        return makeReady(val.getStatus());
    }

    bool isReady() const {
        return _shared->state.load(std::memory_order_acquire) == SSBState::kFinished;
    }

    void wait(Interruptible* interruptible) const {
        _shared->wait(interruptible);
    }

    Status waitNoThrow(Interruptible* interruptible) const noexcept {
        try {
            _shared->wait(interruptible);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        return Status::OK();
    }

    T get(Interruptible* interruptible) && {
        _shared->wait(interruptible);
        uassertStatusOK(std::move(_shared->status));
        return std::move(*(_shared->data));
    }
    T& get(Interruptible* interruptible) & {
        _shared->wait(interruptible);
        uassertStatusOK(_shared->status);
        return *(_shared->data);
    }
    const T& get(Interruptible* interruptible) const& {
        _shared->wait(interruptible);
        uassertStatusOK(_shared->status);
        return *(_shared->data);
    }

    StatusWith<T> getNoThrow(Interruptible* interruptible) && noexcept {
        try {
            _shared->wait(interruptible);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        if (!_shared->status.isOK())
            return std::move(_shared->status);
        return std::move(*_shared->data);
    }

    StatusWith<T> getNoThrow(Interruptible* interruptible) const& noexcept {
        try {
            _shared->wait(interruptible);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        if (!_shared->status.isOK())
            return _shared->status;
        return *_shared->data;
    }

    SharedState<T>* getPtr() {
        return _shared.get();
    }

    SharedState<T>* operator->() {
        return _shared.operator->();
    }

private:
    boost::intrusive_ptr<SharedState<T>> _shared;
};

template <>
class SharedStateHolder<void> {
    using Impl = SharedStateHolder<FakeVoid>;

public:
    explicit SharedStateHolder() : SharedStateHolder(makeReady()) {}
    explicit SharedStateHolder(const boost::intrusive_ptr<SharedState<FakeVoid>>& shared)
        : _inner(shared) {}
    explicit SharedStateHolder(boost::intrusive_ptr<SharedState<FakeVoid>>&& shared)
        : _inner(std::move(shared)) {}
    /*implicit*/ SharedStateHolder(Impl&& shared) : _inner(std::move(shared)) {}
    /*implicit*/ operator Impl &&() && {
        return std::move(_inner);
    }

    static SharedStateHolder makeReady(FakeVoid = {}) {
        return SharedStateHolder<FakeVoid>::makeReady(FakeVoid{});
    }

    static SharedStateHolder makeReady(Status status) {
        if (status.isOK())
            return makeReady();
        return SharedStateHolder<FakeVoid>::makeReady(std::move(status));
    }

    static SharedStateHolder<void> makeReady(StatusWith<FakeVoid> status) {
        return SharedStateHolder<FakeVoid>::makeReady(std::move(status));
    }

    bool isReady() const {
        return _inner.isReady();
    }

    void wait(Interruptible* interruptible) const {
        _inner.wait(interruptible);
    }

    Status waitNoThrow(Interruptible* interruptible) const noexcept {
        return _inner.waitNoThrow(interruptible);
    }

    void get(Interruptible* interruptible) && {
        std::move(_inner).get(interruptible);
    }
    void get(Interruptible* interruptible) const& {
        _inner.get(interruptible);
    }

    Status getNoThrow(Interruptible* interruptible) && noexcept {
        return std::move(_inner).getNoThrow(interruptible).getStatus();
    }
    Status getNoThrow(Interruptible* interruptible) const& noexcept {
        return _inner.getNoThrow(interruptible).getStatus();
    }

private:
    SharedStateHolder<FakeVoid> _inner;
};

template <typename T>
class MONGO_WARN_UNUSED_RESULT_CLASS FutureImpl {
public:
    using value_type = T;

    FutureImpl() = default;

    FutureImpl& operator=(FutureImpl&&) = default;
    FutureImpl(FutureImpl&&) = default;

    FutureImpl(const FutureImpl&) = delete;
    FutureImpl& operator=(const FutureImpl&) = delete;

    explicit FutureImpl(SharedStateHolder<T>&& ptr) : _shared(std::move(ptr)) {}

    static FutureImpl<T> makeReady(T val) {  // TODO emplace?
        FutureImpl out;
        out._immediate = std::move(val);
        return out;
    }

    static FutureImpl<T> makeReady(Status status) {
        return FutureImpl(SharedStateHolder<T>::makeReady(std::move(status)));
    }

    static FutureImpl<T> makeReady(StatusWith<T> val) {
        if (val.isOK())
            return makeReady(std::move(val.getValue()));
        return makeReady(val.getStatus());
    }

    SharedSemiFuture<FakeVoidToVoid<T>> share() && noexcept;

    bool isReady() const {
        return _immediate || _shared.isReady();
    }

    void wait(Interruptible* interruptible) const {
        if (_immediate)
            return;
        _shared.wait(interruptible);
    }

    Status waitNoThrow(Interruptible* interruptible) const noexcept {
        if (_immediate)
            return Status::OK();
        return _shared.waitNoThrow(interruptible);
    }

    T get(Interruptible* interruptible) && {
        if (_immediate)
            return std::move(*_immediate);
        return std::move(_shared).get(interruptible);
    }
    T& get(Interruptible* interruptible) & {
        if (_immediate)
            return *_immediate;
        return _shared.get(interruptible);
    }
    const T& get(Interruptible* interruptible) const& {
        if (_immediate)
            return *_immediate;
        return _shared.get(interruptible);
    }

    StatusWith<T> getNoThrow(Interruptible* interruptible) && noexcept {
        if (_immediate)
            return std::move(*_immediate);
        return std::move(_shared).getNoThrow(interruptible);
    }
    StatusWith<T> getNoThrow(Interruptible* interruptible) const& noexcept {
        if (_immediate)
            return *_immediate;
        return _shared.getNoThrow(interruptible);
    }

    template <typename Func>
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
                _shared->callback = [func = std::forward<Func>(func)](SharedStateBase *
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

    template <typename Func,
              typename Result = NormalizedCallResult<Func, T>,
              typename = std::enable_if_t<!isFuture<Result>>>
        FutureImpl<Result> then(Func&& func) && noexcept {
        return generalImpl(
            // on ready success:
            [&](T&& val) {
                return FutureImpl<Result>::makeReady(statusCall(func, std::move(val)));
            },
            // on ready failure:
            [&](Status&& status) { return FutureImpl<Result>::makeReady(std::move(status)); },
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

    template <typename Func,
              typename RawResult = NormalizedCallResult<Func, T>,
              typename = std::enable_if_t<isFuture<RawResult>>,
              typename UnwrappedResult = typename RawResult::value_type>
        FutureImpl<UnwrappedResult> then(Func&& func) && noexcept {
        return generalImpl(
            // on ready success:
            [&](T&& val) {
                try {
                    return FutureImpl<UnwrappedResult>(throwingCall(func, std::move(val)));
                } catch (const DBException& ex) {
                    return FutureImpl<UnwrappedResult>::makeReady(ex.toStatus());
                }
            },
            // on ready failure:
            [&](Status&& status) {
                return FutureImpl<UnwrappedResult>::makeReady(std::move(status));
            },
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

    template <typename Func,
              typename Result = NormalizedCallResult<Func, Status>,
              typename = std::enable_if_t<!isFuture<Result>>>
        FutureImpl<Result> onCompletion(Func&& func) && noexcept {
        static_assert(std::is_same<Result, NormalizedCallResult<Func, T>>::value,
                      "func passed to Future<T>::onCompletion must return the same type for "
                      "arguments of Status and T");

        return generalImpl(
            // on ready success:
            [&](T&& val) {
                return FutureImpl<Result>::makeReady(
                    statusCall(std::forward<Func>(func), std::move(val)));
            },
            // on ready failure:
            [&](Status&& status) {
                return FutureImpl<Result>::makeReady(
                    statusCall(std::forward<Func>(func), std::move(status)));
            },
            // on not ready yet:
            [&] {
                return makeContinuation<Result>([func = std::forward<Func>(func)](
                    SharedState<T> * input, SharedState<Result> * output) mutable noexcept {
                    if (!input->status.isOK())
                        return output->setFromStatusWith(
                            statusCall(func, std::move(input->status)));

                    output->setFromStatusWith(statusCall(func, std::move(*input->data)));
                });
            });
    }

    template <typename Func,
              typename RawResult = NormalizedCallResult<Func, Status>,
              typename = std::enable_if_t<isFuture<RawResult>>,
              typename UnwrappedResult = typename RawResult::value_type>
        FutureImpl<UnwrappedResult> onCompletion(Func&& func) && noexcept {
        static_assert(std::is_same<UnwrappedResult,
                                   typename NormalizedCallResult<Func, T>::value_type>::value,
                      "func passed to Future<T>::onCompletion must return the same type for "
                      "arguments of Status and T");

        return generalImpl(
            // on ready success:
            [&](T&& val) {
                try {
                    return FutureImpl<UnwrappedResult>(
                        throwingCall(std::forward<Func>(func), std::move(val)));
                } catch (const DBException& ex) {
                    return FutureImpl<UnwrappedResult>::makeReady(ex.toStatus());
                }
            },
            // on ready failure:
            [&](Status&& status) {
                try {
                    return FutureImpl<UnwrappedResult>(
                        throwingCall(std::forward<Func>(func), std::move(status)));
                } catch (const DBException& ex) {
                    return FutureImpl<UnwrappedResult>::makeReady(ex.toStatus());
                }
            },
            // on not ready yet:
            [&] {
                return makeContinuation<UnwrappedResult>([func = std::forward<Func>(func)](
                    SharedState<T> * input,
                    SharedState<UnwrappedResult> * output) mutable noexcept {
                    if (!input->status.isOK()) {
                        try {
                            throwingCall(func, std::move(input->status)).propagateResultTo(output);
                        } catch (const DBException& ex) {
                            output->setError(ex.toStatus());
                        }

                        return;
                    }

                    try {
                        throwingCall(func, std::move(*input->data)).propagateResultTo(output);
                    } catch (const DBException& ex) {
                        output->setError(ex.toStatus());
                    }
                });
            });
    }

    template <typename Func,
              typename Result = RawNormalizedCallResult<Func, Status>,
              typename = std::enable_if_t<!isFuture<Result>>>
        FutureImpl<FakeVoidToVoid<T>> onError(Func&& func) && noexcept {
        static_assert(
            std::is_same<Result, T>::value,
            "func passed to Future<T>::onError must return T, StatusWith<T>, or Future<T>");

        return generalImpl(
            // on ready success:
            [&](T&& val) { return FutureImpl<T>::makeReady(std::move(val)); },
            // on ready failure:
            [&](Status&& status) {
                return FutureImpl<T>::makeReady(statusCall(func, std::move(status)));
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

    template <typename Func,
              typename Result = RawNormalizedCallResult<Func, Status>,
              typename = std::enable_if_t<isFuture<Result>>,
              typename = void>
        FutureImpl<FakeVoidToVoid<T>> onError(Func&& func) && noexcept {
        static_assert(
            std::is_same_v<VoidToFakeVoid<UnwrappedType<Result>>, T>,
            "func passed to Future<T>::onError must return T, StatusWith<T>, or Future<T>");

        return generalImpl(
            // on ready success:
            [&](T&& val) { return FutureImpl<T>::makeReady(std::move(val)); },
            // on ready failure:
            [&](Status&& status) {
                try {
                    return FutureImpl<T>(throwingCall(func, std::move(status)));
                } catch (const DBException& ex) {
                    return FutureImpl<T>::makeReady(ex.toStatus());
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

    template <ErrorCodes::Error code, typename Func>
        FutureImpl<FakeVoidToVoid<T>> onError(Func&& func) && noexcept {
        using Result = RawNormalizedCallResult<Func, Status>;
        static_assert(
            std::is_same_v<VoidToFakeVoid<UnwrappedType<Result>>, T>,
            "func passed to Future<T>::onError must return T, StatusWith<T>, or Future<T>");

        if (_immediate || (isReady() && _shared->status.isOK()))
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

    template <ErrorCategory category, typename Func>
        FutureImpl<FakeVoidToVoid<T>> onErrorCategory(Func&& func) && noexcept {
        using Result = RawNormalizedCallResult<Func, Status>;
        static_assert(
            std::is_same<Result, T>::value || std::is_same<Result, FutureImpl<T>>::value ||
                (std::is_same<T, FakeVoid>::value && std::is_same<Result, FutureImpl<void>>::value),
            "func passed to Future<T>::onErrorCategory must return T, StatusWith<T>, or Future<T>");

        if (_immediate || (isReady() && _shared->status.isOK()))
            return std::move(*this);

        return std::move(*this).onError([func =
                                             std::forward<Func>(func)](Status && status) mutable {
            if (!ErrorCodes::isA<category>(status.code()))
                uassertStatusOK(status);
            return throwingCall(func, std::move(status));
        });
    }

    template <typename Func>
        FutureImpl<FakeVoidToVoid<T>> tap(Func&& func) && noexcept {
        static_assert(std::is_void<decltype(call(func, std::declval<const T&>()))>::value,
                      "func passed to tap must return void");

        return tapImpl(std::forward<Func>(func),
                       [](Func && func, const T& val) noexcept { call(func, val); },
                       [](Func && func, const Status& status) noexcept {});
    }

    template <typename Func>
        FutureImpl<FakeVoidToVoid<T>> tapError(Func&& func) && noexcept {
        static_assert(std::is_void<decltype(call(func, std::declval<const Status&>()))>::value,
                      "func passed to tapError must return void");

        return tapImpl(std::forward<Func>(func),
                       [](Func && func, const T& val) noexcept {},
                       [](Func && func, const Status& status) noexcept { call(func, status); });
    }

    template <typename Func>
        FutureImpl<FakeVoidToVoid<T>> tapAll(Func&& func) && noexcept {
        static_assert(std::is_void<decltype(call(func, std::declval<const T&>()))>::value,
                      "func passed to tapAll must return void");
        static_assert(std::is_void<decltype(call(func, std::declval<const Status&>()))>::value,
                      "func passed to tapAll must return void");

        return tapImpl(std::forward<Func>(func),
                       [](Func && func, const T& val) noexcept { call(func, val); },
                       [](Func && func, const Status& status) noexcept { call(func, status); });
    }

    FutureImpl<void> ignoreValue() && noexcept;

    void propagateResultTo(SharedState<T>* output) && noexcept {
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
                    _shared->continuation = std::move(output->continuation);
                } else {
                    _shared->continuation = output;
                }
                _shared->isJustForContinuation.store(true, std::memory_order_release);

                _shared->callback = [](SharedStateBase * ssb) noexcept {
                    const auto input = checked_cast<SharedState<T>*>(ssb);
                    const auto output = checked_cast<SharedState<T>*>(ssb->continuation.get());
                    output->fillFrom(std::move(*input));
                };
            });
    }

private:
    template <typename>
    friend class FutureImpl;
    friend class Promise<T>;
    friend class SharedPromise<T>;
    friend class SharedSemiFuture<FakeVoidToVoid<T>>;

    // All callbacks are called immediately so they are allowed to capture everything by reference.
    // All callbacks should return the same return type.
    template <typename SuccessFunc, typename FailFunc, typename NotReady>
    auto generalImpl(SuccessFunc&& success, FailFunc&& fail, NotReady&& notReady) noexcept {
        if (_immediate) {
            return success(std::move(*_immediate));
        }

        auto oldState = _shared->state.load(std::memory_order_acquire);
        dassert(oldState != SSBState::kHaveContinuation);
        if (oldState == SSBState::kFinished) {
            if (_shared->status.isOK()) {
                return success(std::move(*_shared->data));
            } else {
                return fail(std::move(_shared->status));
            }
        }

        // This is always done after notReady, which never throws. It is in an ON_BLOCK_EXIT to
        // support both void- and value-returning notReady implementations since we can't assign
        // void to a variable.
        ON_BLOCK_EXIT([&] {
            // oldState could be either kInit or kWaiting, depending on whether we've failed a call
            // to wait().
            if (MONGO_unlikely(!_shared->state.compare_exchange_strong(
                    oldState, SSBState::kHaveContinuation, std::memory_order_acq_rel))) {
                dassert(oldState == SSBState::kFinished);
                _shared->callback(_shared.getPtr());
            }
        });

        return notReady();
    }

    // success and fail may be called from a continuation so they shouldn't capture anything.
    template <typename Callback, typename SuccessFunc, typename FailFunc>
    FutureImpl<FakeVoidToVoid<T>> tapImpl(Callback&& cb,
                                          SuccessFunc&& success,
                                          FailFunc&& fail) noexcept {
        // Make sure they don't capture anything.
        MONGO_STATIC_ASSERT(std::is_empty<SuccessFunc>::value);
        MONGO_STATIC_ASSERT(std::is_empty<FailFunc>::value);

        return generalImpl(
            [&](T&& val) {
                success(std::forward<Callback>(cb), stdx::as_const(val));
                return FutureImpl<T>::makeReady(std::move(val));
            },
            [&](Status&& status) {
                fail(std::forward<Callback>(cb), stdx::as_const(status));
                return FutureImpl<T>::makeReady(std::move(status));
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

    template <typename Result, typename OnReady>
    inline FutureImpl<Result> makeContinuation(OnReady&& onReady) {
        invariant(!_shared->callback && !_shared->continuation);

        auto continuation = make_intrusive<SharedState<Result>>();
        continuation->threadUnsafeIncRefCountTo(2);
        _shared->continuation.reset(continuation.get(), /*add ref*/ false);
        _shared->callback = [onReady = std::forward<OnReady>(onReady)](SharedStateBase *
                                                                       ssb) mutable noexcept {
            const auto input = checked_cast<SharedState<T>*>(ssb);
            const auto output = checked_cast<SharedState<Result>*>(ssb->continuation.get());
            onReady(input, output);
        };
        return FutureImpl<Result>(SharedStateHolder<Result>(std::move(continuation)));
    }

    // At most one of these will be active.
    boost::optional<T> _immediate;
    SharedStateHolder<T> _shared;
};

template <>
class MONGO_WARN_UNUSED_RESULT_CLASS FutureImpl<void> : public FutureImpl<FakeVoid> {
    using Base = FutureImpl<FakeVoid>;

public:
    using value_type = void;

    FutureImpl() : FutureImpl(makeReady()) {}

    explicit FutureImpl(SharedStateHolder<FakeVoid>&& holder) : Base(std::move(holder)) {}
    /*implicit*/ FutureImpl(FutureImpl<FakeVoid>&& inner) : Base(std::move(inner)) {}

    // Only replacing a few methods to use void/Status in place of FakeVoid. The callback method
    // fixups are handled by call().

    static FutureImpl<void> makeReady() {
        return FutureImpl<FakeVoid>::makeReady(FakeVoid{});
    }

    static FutureImpl<void> makeReady(Status status) {
        if (status.isOK())
            return makeReady();
        return Base::makeReady(std::move(status));
    }

    static FutureImpl<void> makeReady(StatusWith<FakeVoid> status) {
        return Base::makeReady(std::move(status));
    }

    void get(Interruptible* interruptible) && {
        std::move(base()).get(interruptible);
    }
    void get(Interruptible* interruptible) const& {
        base().get(interruptible);
    }

    Status getNoThrow(Interruptible* interruptible) && noexcept {
        return std::move(base()).getNoThrow(interruptible).getStatus();
    }
    Status getNoThrow(Interruptible* interruptible) const& noexcept {
        return base().getNoThrow(interruptible).getStatus();
    }

    FutureImpl<void> ignoreValue() && noexcept {
        return std::move(*this);
    }

private:
    Base& base() {
        return *this;
    }
    const Base& base() const {
        return *this;
    }
};

template <typename T>
    inline FutureImpl<void> FutureImpl<T>::ignoreValue() && noexcept {
    return std::move(*this).then([](auto&&) {});
}


}  // namespace future_details
}  // namespace mongo
