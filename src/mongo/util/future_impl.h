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
#include <forward_list>
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
#include "mongo/util/if_constexpr.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
template <typename T>
class Promise;

template <typename T>
class Future;

template <typename T>
class SemiFuture;

template <typename T>
class ExecutorFuture;

template <typename T>
class SharedPromise;

template <typename T>
class SharedSemiFuture;

namespace future_details {

template <typename T>
class FutureImpl;
template <>
class FutureImpl<void>;

template <typename T>
inline constexpr bool isFutureLike = false;
template <typename T>
inline constexpr bool isFutureLike<Future<T>> = true;
template <typename T>
inline constexpr bool isFutureLike<SemiFuture<T>> = true;
template <typename T>
inline constexpr bool isFutureLike<ExecutorFuture<T>> = true;
template <typename T>
inline constexpr bool isFutureLike<SharedSemiFuture<T>> = true;

template <typename T>
struct UnstatusTypeImpl {
    using type = T;
};
template <typename T>
struct UnstatusTypeImpl<StatusWith<T>> {
    using type = T;
};
template <>
struct UnstatusTypeImpl<Status> {
    using type = void;
};
template <typename T>
using UnstatusType = typename UnstatusTypeImpl<T>::type;

template <typename T>
struct UnwrappedTypeImpl {
    static_assert(!isFutureLike<T>);
    static_assert(!isStatusOrStatusWith<T>);
    using type = T;
};
template <typename T>
struct UnwrappedTypeImpl<Future<T>> {
    using type = T;
};
template <typename T>
struct UnwrappedTypeImpl<SemiFuture<T>> {
    using type = T;
};
template <typename T>
struct UnwrappedTypeImpl<ExecutorFuture<T>> {
    using type = T;
};
template <typename T>
struct UnwrappedTypeImpl<SharedSemiFuture<T>> {
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
using UnwrappedType = typename UnwrappedTypeImpl<T>::type;

template <typename T>
struct FutureContinuationKindImpl {
    static_assert(!isFutureLike<T>);
    using type = Future<T>;
};
template <typename T>
struct FutureContinuationKindImpl<Future<T>> {
    using type = Future<T>;
};
template <typename T>
struct FutureContinuationKindImpl<SemiFuture<T>> {
    using type = SemiFuture<T>;
};
template <typename T>
struct FutureContinuationKindImpl<ExecutorFuture<T>> {
    // Weird but right. ExecutorFuture needs to know the executor prior to running the continuation,
    // and in this case it doesn't.
    using type = SemiFuture<T>;
};
template <typename T>
struct FutureContinuationKindImpl<SharedSemiFuture<T>> {
    using type = SemiFuture<T>;  // It will generate a child continuation.
};
template <typename T>
using FutureContinuationKind = typename FutureContinuationKindImpl<T>::type;

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

struct InvalidCallSentinal;  // Nothing actually returns this.
template <typename Func, typename Arg, typename = void>
struct FriendlyInvokeResultImpl {
    using type = InvalidCallSentinal;
};
template <typename Func, typename Arg>
struct FriendlyInvokeResultImpl<
    Func,
    Arg,
    std::enable_if_t<std::is_invocable_v<Func, std::enable_if_t<!std::is_void_v<Arg>, Arg>>>> {
    using type = std::invoke_result_t<Func, Arg>;
};
template <typename Func>
struct FriendlyInvokeResultImpl<Func, void, std::enable_if_t<std::is_invocable_v<Func>>> {
    using type = std::invoke_result_t<Func>;
};
template <typename Func>
struct FriendlyInvokeResultImpl<Func, const void, std::enable_if_t<std::is_invocable_v<Func>>> {
    using type = std::invoke_result_t<Func>;
};

template <typename Func, typename Arg>
using FriendlyInvokeResult = typename FriendlyInvokeResultImpl<Func, Arg>::type;

// Like is_invocable_v<Func, Args>, but handles Args == void correctly.
template <typename Func, typename Arg>
inline constexpr bool isCallable =
    !std::is_same_v<FriendlyInvokeResult<Func, Arg>, InvalidCallSentinal>;

// Like is_invocable_r_v<Func, Args>, but handles Args == void correctly and unwraps the return.
template <typename Ret, typename Func, typename Arg>
inline constexpr bool isCallableR =
    (isCallable<Func, Arg> && std::is_same_v<UnwrappedType<FriendlyInvokeResult<Func, Arg>>, Ret>);

// Like isCallableR, but doesn't unwrap the result type.
template <typename Ret, typename Func, typename Arg>
inline constexpr bool isCallableExactR = (isCallable<Func, Arg> &&
                                          std::is_same_v<FriendlyInvokeResult<Func, Arg>, Ret>);

/**
 * call() normalizes arguments to hide the FakeVoid shenanigans from users of Futures.
 * In the future it may also expand tuples to argument lists.
 */
template <typename Func, typename Arg>
inline auto call(Func&& func, Arg&& arg) {
    return func(std::forward<Arg>(arg));
}

template <typename Func>
inline auto call(Func&& func, FakeVoid) {
    return func();
}

template <typename Func>
inline auto call(Func&& func, StatusWith<FakeVoid> sw) {
    return func(sw.getStatus());
}

/**
 * statusCall() normalizes return values so everything returns StatusWith<T>. Exceptions are
 * converted to !OK statuses. void and Status returns are converted to StatusWith<FakeVoid>
 */
template <typename Func, typename... Args>
inline auto statusCall(Func&& func, Args&&... args) noexcept {
    using RawResult = decltype(call(func, std::forward<Args>(args)...));
    using Result = StatusWith<VoidToFakeVoid<UnstatusType<RawResult>>>;
    try {
        IF_CONSTEXPR(std::is_void_v<RawResult>) {
            call(func, std::forward<Args>(args)...);
            return Result(FakeVoid());
        }
        else IF_CONSTEXPR(std::is_same_v<RawResult, Status>) {
            auto s = call(func, std::forward<Args>(args)...);
            if (!s.isOK()) {
                return Result(std::move(s));
            }
            return Result(FakeVoid());
        }
        else {
            return Result(call(func, std::forward<Args>(args)...));
        }
    } catch (const DBException& ex) {
        return Result(ex.toStatus());
    }
}

/**
 * throwingCall() normalizes return values so everything returns T or FakeVoid. !OK Statuses are
 * converted exceptions. void and Status returns are converted to FakeVoid.
 *
 * This is equivalent to uassertStatusOK(statusCall(func, args...)), but avoids catching just to
 * rethrow.
 */
template <typename Func, typename... Args>
inline auto throwingCall(Func&& func, Args&&... args) {
    using Result = decltype(call(func, std::forward<Args>(args)...));
    IF_CONSTEXPR(std::is_void_v<Result>) {
        call(func, std::forward<Args>(args)...);
        return FakeVoid{};
    }
    else IF_CONSTEXPR(std::is_same_v<Result, Status>) {
        uassertStatusOK(call(func, std::forward<Args>(args)...));
        return FakeVoid{};
    }
    else IF_CONSTEXPR(isStatusWith<Result>) {
        return uassertStatusOK(call(func, std::forward<Args>(args)...));
    }
    else {
        return call(func, std::forward<Args>(args)...);
    }
}

template <typename Func, typename... Args>
using NormalizedCallResult = FakeVoidToVoid<
    UnstatusType<decltype(call(std::declval<Func>(), std::declval<VoidToFakeVoid<Args>>()...))>>;

template <typename T>
struct SharedStateImpl;

template <typename T>
using SharedState = SharedStateImpl<VoidToFakeVoid<T>>;

/**
 * SSB is SharedStateBase, and this is its current state.
 *
 * Legal transitions on future side:
 *      kInit -> kWaitingOrHaveChildren
 *      kInit -> kHaveCallback
 *      kWaitingOrHaveChildren -> kHaveCallback
 *
 * Legal transitions on promise side:
 *      kInit -> kFinished
 *      kWaitingOrHaveChildren -> kFinished
 *      kHaveCallback -> kFinished
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

    // Promise hasn't been completed. Either someone has constructed the condvar and may be waiting
    // on it, or children is non-empty. Either way, the completer of the promise must acquire the
    // mutex inside transitionToFinished() to determine what needs to be done. We do not transition
    // back to kInit if they give up on waiting. There is also no callback directly registered in
    // this state, although callbacks may be registered on children.
    kWaitingOrHaveChildren,

    // Promise hasn't been completed. Someone has registered a callback to be run when it is.
    // There is no-one currently waiting on the condvar, and there are no children. Once a future is
    // shared, its state can never transition to this.
    kHaveCallback,

    // The promise has been completed with a value or error. This is the terminal state. This should
    // stay last since we have code like assert(state < kFinished).
    kFinished,
};

class SharedStateBase : public RefCountable {
public:
    using Children = std::forward_list<boost::intrusive_ptr<SharedStateBase>>;

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
            // We don't need release (or acq_rel) here because the cv construction will be released
            // and acquired via the mutex.
            if (MONGO_unlikely(!state.compare_exchange_strong(
                    oldState, SSBState::kWaitingOrHaveChildren, std::memory_order_acquire))) {
                if (oldState == SSBState::kFinished) {
                    // transitionToFinished() transitioned after we did our initial check.
                    return;
                }
                // Someone else did this transition.
                invariant(oldState == SSBState::kWaitingOrHaveChildren);
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

        dassert(oldState == SSBState::kWaitingOrHaveChildren ||
                oldState == SSBState::kHaveCallback);

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
                 ssb = ssb->state.load(std::memory_order_acquire) == SSBState::kHaveCallback
                     ? ssb->continuation.get()
                     : nullptr) {
                depth++;

                invariant(depth < kMaxDepth);
            }
        }

        if (oldState == SSBState::kHaveCallback) {
            dassert(children.empty());
            callback(this);
        } else {
            invariant(!callback);

            Children localChildren;

            stdx::unique_lock<stdx::mutex> lk(mx);
            localChildren.swap(children);
            if (cv) {
                // This must be done inside the lock to correctly synchronize with wait().
                cv->notify_all();
            }
            lk.unlock();

            if (!localChildren.empty()) {
                fillChildren(localChildren);
            }
        }
    }

    virtual void fillChildren(const Children&) const = 0;

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
    // kWaitingOrHaveChildren from Future. Promise can acquire access to all members by doing an
    // acquire-load of state and seeing kWaitingOrHaveChildren (or Future with kFinished).
    // Transitions should be done via acquire-release exchanges to combine both actions.
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
    boost::optional<stdx::condition_variable> cv;  // F (but guarded by mutex)

    // This holds the children created from a SharedSemiFuture. When this SharedState is completed,
    // the result will be copied in to each of the children. This allows their continuations to have
    // their own mutable copy, rather than tracking mutability for each callback.
    Children children;  // F (but guarded by mutex)

    Status status = Status::OK();  // P
protected:
    SharedStateBase() = default;
};

template <typename T>
struct SharedStateImpl final : SharedStateBase {
    static_assert(!std::is_void<T>::value);

    // Initial methods only called from future side.

    boost::intrusive_ptr<SharedState<T>> addChild() {
        static_assert(std::is_copy_constructible_v<T>);  // T has been through VoidToFakeVoid.
        invariant(!callback);

        auto out = make_intrusive<SharedState<T>>();
        if (state.load(std::memory_order_acquire) == SSBState::kFinished) {
            out->fillFromConst(*this);
            return out;
        }

        auto lk = stdx::unique_lock(mx);
        auto oldState = state.load(std::memory_order_acquire);
        if (oldState == SSBState::kFinished) {
            lk.unlock();
            out->fillFromConst(*this);
            return out;
        }

        if (oldState == SSBState::kInit) {
            // memory ordering can be relaxed because that will be handled by the mutex.
            state.compare_exchange_strong(
                oldState, SSBState::kWaitingOrHaveChildren, std::memory_order_relaxed);
        }
        dassert(oldState != SSBState::kHaveCallback);

        // If oldState became kFinished after we checked (and therefore after we acquired the lock),
        // the returned continuation will be completed by the promise side once it acquires the lock
        // since we are adding ourself to the chain here.

        children.emplace_front(out.get(), /*add ref*/ false);
        out->threadUnsafeIncRefCountTo(2);
        return out;
    }

    // Remaining methods only called by promise side.

    // fillFromConst and fillFromMove are identical other than using as_const() vs move().
    void fillFromConst(const SharedState<T>& other) {
        dassert(state.load() < SSBState::kFinished);
        dassert(other.state.load() == SSBState::kFinished);
        if (other.status.isOK()) {
            data.emplace(std::as_const(*other.data));
        } else {
            status = std::as_const(other.status);
        }
        transitionToFinished();
    }
    void fillFromMove(SharedState<T>&& other) {
        dassert(state.load() < SSBState::kFinished);
        dassert(other.state.load() == SSBState::kFinished);
        if (other.status.isOK()) {
            data.emplace(std::move(*other.data));
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

    void fillChildren(const Children& children) const override {
        IF_CONSTEXPR(std::is_copy_constructible_v<T>) {  // T has been through VoidToFakeVoid.
            for (auto&& child : children) {
                checked_cast<SharedState<T>*>(child.get())->fillFromConst(*this);
            }
        }
        else {
            invariant(false, "should never call fillChildren with non-copyable T");
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

    SharedStateHolder<VoidToFakeVoid<T>> addChild() const {
        return SharedStateHolder<VoidToFakeVoid<T>>(_shared->addChild());
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

    SharedStateHolder<VoidToFakeVoid<void>> addChild() const {
        return _inner.addChild();
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
            [&](T&& val) { call(func, StatusWith<T>(std::move(val))); },
            // on ready failure:
            [&](Status&& status) { call(func, StatusWith<T>(std::move(status))); },
            // on not ready yet:
            [&] {
                _shared->callback = [func = std::forward<Func>(func)](SharedStateBase *
                                                                      ssb) mutable noexcept {
                    const auto input = checked_cast<SharedState<T>*>(ssb);
                    if (input->status.isOK()) {
                        call(func, StatusWith<T>(std::move(*input->data)));
                    } else {
                        call(func, StatusWith<T>(std::move(input->status)));
                    }
                };
            });
    }

    template <typename Func>
        auto then(Func&& func) && noexcept {
        using Result = NormalizedCallResult<Func, T>;
        IF_CONSTEXPR(!isFutureLike<Result>) {
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
        else {
            using UnwrappedResult = typename Result::value_type;
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
    }

    template <typename Func>
        auto onCompletion(Func&& func) && noexcept {
        using Wrapper = StatusOrStatusWith<T>;
        using Result = NormalizedCallResult<Func, StatusOrStatusWith<T>>;
        IF_CONSTEXPR(!isFutureLike<Result>) {
            return generalImpl(
                // on ready success:
                [&](T&& val) {
                    return FutureImpl<Result>::makeReady(
                        statusCall(std::forward<Func>(func), Wrapper(std::move(val))));
                },
                // on ready failure:
                [&](Status&& status) {
                    return FutureImpl<Result>::makeReady(
                        statusCall(std::forward<Func>(func), Wrapper(std::move(status))));
                },
                // on not ready yet:
                [&] {
                    return makeContinuation<Result>([func = std::forward<Func>(func)](
                        SharedState<T> * input, SharedState<Result> * output) mutable noexcept {
                        if (!input->status.isOK())
                            return output->setFromStatusWith(
                                statusCall(func, Wrapper(std::move(input->status))));

                        output->setFromStatusWith(
                            statusCall(func, Wrapper(std::move(*input->data))));
                    });
                });
        }
        else {
            using UnwrappedResult = typename Result::value_type;
            return generalImpl(
                // on ready success:
                [&](T&& val) {
                    try {
                        return FutureImpl<UnwrappedResult>(
                            throwingCall(std::forward<Func>(func), Wrapper(std::move(val))));
                    } catch (const DBException& ex) {
                        return FutureImpl<UnwrappedResult>::makeReady(ex.toStatus());
                    }
                },
                // on ready failure:
                [&](Status&& status) {
                    try {
                        return FutureImpl<UnwrappedResult>(
                            throwingCall(std::forward<Func>(func), Wrapper(std::move(status))));
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
                                throwingCall(func, Wrapper(std::move(input->status)))
                                    .propagateResultTo(output);
                            } catch (const DBException& ex) {
                                output->setError(ex.toStatus());
                            }

                            return;
                        }

                        try {
                            throwingCall(func, Wrapper(std::move(*input->data)))
                                .propagateResultTo(output);
                        } catch (const DBException& ex) {
                            output->setError(ex.toStatus());
                        }
                    });
                });
        }
    }

    template <typename Func>
        FutureImpl<FakeVoidToVoid<T>> onError(Func&& func) && noexcept {
        using Result = NormalizedCallResult<Func, Status>;
        static_assert(
            std::is_same<VoidToFakeVoid<UnwrappedType<Result>>, T>::value,
            "func passed to Future<T>::onError must return T, StatusWith<T>, or Future<T>");

        IF_CONSTEXPR(!isFutureLike<Result>) {
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
        else {
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
    }

    template <ErrorCodes::Error code, typename Func>
        FutureImpl<FakeVoidToVoid<T>> onError(Func&& func) && noexcept {
        using Result = NormalizedCallResult<Func, Status>;
        static_assert(
            std::is_same_v<UnwrappedType<Result>, FakeVoidToVoid<T>>,
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
        using Result = NormalizedCallResult<Func, Status>;
        static_assert(std::is_same_v<UnwrappedType<Result>, FakeVoidToVoid<T>>,
                      "func passed to Future<T>::onErrorCategory must return T, StatusWith<T>, "
                      "or Future<T>");

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
        static_assert(
            std::is_void<decltype(call(func, std::declval<const StatusOrStatusWith<T>&>()))>::value,
            "func passed to tapAll must return void");

        using Wrapper = StatusOrStatusWith<T>;
        return tapImpl(
            std::forward<Func>(func),
            [](Func && func, const T& val) noexcept { call(func, Wrapper(val)); },
            [](Func && func, const Status& status) noexcept { call(func, Wrapper(status)); });
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
                    output->fillFromMove(std::move(*input));
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
        dassert(oldState != SSBState::kHaveCallback);
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
            dassert(_shared->children.empty());
            // oldState could be either kInit or kWaitingOrHaveChildren, depending on whether we've
            // failed a call to wait().
            if (MONGO_unlikely(!_shared->state.compare_exchange_strong(
                    oldState, SSBState::kHaveCallback, std::memory_order_acq_rel))) {
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

                    output->fillFromMove(std::move(*input));
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
