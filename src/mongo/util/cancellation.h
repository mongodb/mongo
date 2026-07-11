// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"
#include "mongo/util/static_immortal.h"

#include <new>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {

namespace detail {

inline Status getCancelNeverCalledOnSourceError() {
    static const StaticImmortal<Status> cancelNeverCalledOnSourceError{
        ErrorCodes::CallbackCanceled,
        "Cancel was never called on the CancellationSource for this token."};
    return *cancelNeverCalledOnSourceError;
}

/**
 * Holds the main state shared between CancellationSource/CancellationToken.
 *
 * CancellationState objects are held by intrusive_ptr, and the ownership of a CancellationState
 * object is shared between all CancellationSource objects and CancellationToken objects which point
 * to it.
 *
 * When the last CancellationSource that points to a CancellationState object is destroyed,
 * CancellationState::dismiss() is called, which sets an error on its cancellation promise if
 * CancellationState::cancel() has not already been called. This serves to clean up the memory for
 * all callbacks associated with that promise once it is no longer possible for cancel() to be
 * called on the source.
 */
class CancellationState : public RefCountable {
    enum class State : int { kInit, kCanceled, kDismissed };

public:
    CancellationState() = default;

    ~CancellationState() override {
        auto state = _state.load();
        invariant(state == State::kCanceled || state == State::kDismissed);
        invariant(_cancellationPromise.getFuture().isReady());
    }

    CancellationState(const CancellationState& other) = delete;
    CancellationState& operator=(const CancellationState& other) = delete;

    CancellationState(CancellationState&& other) = delete;
    CancellationState& operator=(CancellationState&& other) = delete;

    void dismiss() {
        State precondition{State::kInit};
        if (_state.compareAndSwap(&precondition, State::kDismissed)) {
            _cancellationPromise.setError(getCancelNeverCalledOnSourceError());
        }
    }

    void cancel() {
        State precondition{State::kInit};
        if (_state.compareAndSwap(&precondition, State::kCanceled)) {
            _cancellationPromise.emplaceValue();
        }
    }

    bool isCanceled() const {
        return _state.load() == State::kCanceled;
    }

    SharedSemiFuture<void> onCancel() const {
        return _cancellationPromise.getFuture();
    }

    /**
     * Returns true if neither cancel() nor dismiss() has been called.
     */
    bool isCancelable() const {
        return _state.load() == State::kInit;
    }

private:
    /**
     * Tracks whether dismiss/cancel has been called.
     */
    Atomic<State> _state{State::kInit};

    /**
     * A promise that will be signaled with success when cancel() is called and with an error when
     * dismiss() is called.
     */
    SharedPromise<void> _cancellationPromise;
};

/**
 * Wrapper around an intrusive_ptr<CancellationState> which, when destroyed, dismisses the
 * CancellationState. These used to track how many CancellationSource objects point to the same
 * CancellationState and call dismiss() on the CancellationState when the last CancellationSource
 * pointing to it is destroyed.
 */
class CancellationStateHolder : public RefCountable {
public:
    CancellationStateHolder() = default;

    ~CancellationStateHolder() override {
        _state->dismiss();
    }

    CancellationStateHolder(const CancellationStateHolder&) = delete;
    CancellationStateHolder& operator=(const CancellationStateHolder&) = delete;

    CancellationStateHolder(CancellationStateHolder&&) = delete;
    CancellationStateHolder& operator=(CancellationStateHolder&&) = delete;

    boost::intrusive_ptr<CancellationState> get() const {
        return _state;
    }

private:
    boost::intrusive_ptr<CancellationState> _state{make_intrusive<CancellationState>()};
};

}  // namespace detail

/**
 * Type used to check for cancellation of a task. Tokens are normally obtained through an associated
 * CancellationSource by calling CancellationSource::token(), but an uncancelable token can also be
 * constructed by using the CancellationToken::uncancelable() static factory function.
 */
class CancellationToken {
public:
    // Constructs an uncancelable token, i.e. a token without an associated source.
    static CancellationToken uncancelable() {
        auto state = make_intrusive<detail::CancellationState>();
        // Make the state uncancelable.
        state->dismiss();
        return CancellationToken(std::move(state));
    }

    explicit CancellationToken(boost::intrusive_ptr<const detail::CancellationState> state)
        : _state(std::move(state)) {}

    ~CancellationToken() = default;

    CancellationToken(const CancellationToken& other) = default;
    CancellationToken& operator=(const CancellationToken& other) = default;

    CancellationToken(CancellationToken&& other) = default;
    CancellationToken& operator=(CancellationToken&& other) = default;

    /**
     * Returns whether or not cancel() has been called on the CancellationSource object from which
     * this token was constructed.
     */
    bool isCanceled() const {
        return _state->isCanceled();
    }

    /**
     * Returns a future that will be resolved with success when cancel() has been called on the
     * CancellationSource object from which this token was constructed, or with an error containing
     * CallbackCanceled if that CancellationSource object is destroyed without having cancel()
     * called on it.
     */
    SemiFuture<void> onCancel() const {
        return _state->onCancel().semi();
    }

    /**
     * Returns false if this token was constructed using the uncancelable() factory function or if
     * its associated source was destroyed without being canceled.
     */
    bool isCancelable() const {
        return _state->isCancelable();
    }

private:
    /**
     * Points to the object containing the status of cancellation.
     */
    boost::intrusive_ptr<const detail::CancellationState> _state;
};

/**
 * Type used to manage the cancellation of a task. CancellationSource is used to cancel a task, and
 * CancellationTokens obtained via CancellationSource::token() are used to check for and handle
 * cancellation.
 */
class CancellationSource {
public:
    CancellationSource() = default;
    /**
     * Creates a CancellationSource that will be canceled when the input token is canceled. This
     * allows the construction of cancellation hierarchies.
     *
     * For example, if we have:
     *
     * CancellationSource first;
     * CancellationSource second(first.token());
     * CancellationSource third(second.token());
     *
     * Calling third.cancel() will only cancel tokens obtained from third.
     * Calling second.cancel() will cancel tokens obtained from second, and call third.cancel().
     * Calling first.cancel() will thus cancel the whole hierarchy.
     */
    explicit CancellationSource(const CancellationToken& token) {
        // Cancel the source when the input token is canceled.
        //
        // Note that because this captures the CancellationState object directly, and not the
        // CancellationStateHolder, this will still allow callback state attached to this
        // source's tokens to be cleaned up as soon as the last source is destroyed, even if the
        // parent token still exists.. This means that long-lived tokens can have many sub-sources
        // for tasks which start and complete without worrying about too much memory build-up.
        token.onCancel()
            .unsafeToInlineFuture()
            .then([state = this->_stateHolder->get()] { state->cancel(); })
            .getAsync([](auto) {});
    }

    /**
     * Destroys shared state associated with any tokens obtained from this source, and does not run
     * cancellation callbacks.
     */
    ~CancellationSource() = default;

    CancellationSource(const CancellationSource& other) = default;
    CancellationSource& operator=(const CancellationSource& other) = default;

    CancellationSource(CancellationSource&& other) = default;
    CancellationSource& operator=(CancellationSource&& other) = default;

    /**
     * Cancel the token. If no call to cancel has previously been made, this will cause all
     * callbacks chained to this token via the onCancel future to be run. Otherwise, does nothing.
     */
    void cancel() {
        _stateHolder->get()->cancel();
    }

    /**
     * Returns a CancellationToken which will be canceled when this source is
     * canceled.
     */
    CancellationToken token() const {
        return CancellationToken{_stateHolder->get()};
    }

private:
    boost::intrusive_ptr<detail::CancellationStateHolder> _stateHolder{
        make_intrusive<detail::CancellationStateHolder>()};
};

}  // namespace mongo
