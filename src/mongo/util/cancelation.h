/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/future.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

namespace detail {

inline Status getCancelNeverCalledOnSourceError() {
    static const StaticImmortal<Status> cancelNeverCalledOnSourceError{
        ErrorCodes::CallbackCanceled,
        "Cancel was never called on the CancelationSource for this token."};
    return *cancelNeverCalledOnSourceError;
}

/**
 * Holds the main state shared between CancelationSource/CancelationToken.
 *
 * CancelationState objects are held by intrusive_ptr, and the ownership of a CancelationState
 * object is shared between all CancelationSource objects and CancelationToken objects which point
 * to it.
 *
 * When the last CancelationSource that points to a CancelationState object is destroyed,
 * CancelationState::dismiss() is called, which sets an error on its cancelation promise if
 * CancelationState::cancel() has not already been called. This serves to clean up the memory for
 * all callbacks associated with that promise once it is no longer possible for cancel() to be
 * called on the source.
 */
class CancelationState : public RefCountable {
    enum class State : int { kInit, kCanceled, kDismissed };

public:
    CancelationState() = default;

    ~CancelationState() {
        auto state = _state.load();
        invariant(state == State::kCanceled || state == State::kDismissed);
        invariant(_cancelationPromise.getFuture().isReady());
    }

    CancelationState(const CancelationState& other) = delete;
    CancelationState& operator=(const CancelationState& other) = delete;

    CancelationState(CancelationState&& other) = delete;
    CancelationState& operator=(CancelationState&& other) = delete;

    void dismiss() {
        State precondition{State::kInit};
        if (_state.compareAndSwap(&precondition, State::kDismissed)) {
            _cancelationPromise.setError(getCancelNeverCalledOnSourceError());
        }
    }

    void cancel() {
        State precondition{State::kInit};
        if (_state.compareAndSwap(&precondition, State::kCanceled)) {
            _cancelationPromise.emplaceValue();
        }
    }

    bool isCanceled() const {
        return _state.loadRelaxed() == State::kCanceled;
    }

    SharedSemiFuture<void> onCancel() const {
        return _cancelationPromise.getFuture();
    }

    /**
     * Returns true if neither cancel() nor dismiss() has been called.
     */
    bool isCancelable() const {
        return _state.loadRelaxed() == State::kInit;
    }

private:
    /**
     * Tracks whether dismiss/cancel has been called.
     */
    AtomicWord<State> _state{State::kInit};

    /**
     * A promise that will be signaled with success when cancel() is called and with an error when
     * dismiss() is called.
     */
    SharedPromise<void> _cancelationPromise;
};

/**
 * Wrapper around an intrusive_ptr<CancelationState> which, when destroyed, dismisses the
 * CancelationState. These used to track how many CancelationSource objects point to the same
 * CancelationState and call dismiss() on the CancelationState when the last CancelationSource
 * pointing to it is destroyed.
 */
class CancelationStateHolder : public RefCountable {
public:
    CancelationStateHolder() = default;

    ~CancelationStateHolder() {
        _state->dismiss();
    }

    CancelationStateHolder(const CancelationStateHolder&) = delete;
    CancelationStateHolder& operator=(const CancelationStateHolder&) = delete;

    CancelationStateHolder(CancelationStateHolder&&) = delete;
    CancelationStateHolder& operator=(CancelationStateHolder&&) = delete;

    boost::intrusive_ptr<CancelationState> get() const {
        return _state;
    }

private:
    boost::intrusive_ptr<CancelationState> _state{make_intrusive<CancelationState>()};
};

}  // namespace detail

/**
 * Type used to check for cancelation of a task. Tokens are normally obtained through an associated
 * CancelationSource by calling CancelationSource::token(), but an uncancelable token can also be
 * constructed by using the CancelationToken::uncancelable() static factory function.
 */
class CancelationToken {
public:
    // Constructs an uncancelable token, i.e. a token without an associated source.
    static CancelationToken uncancelable() {
        auto state = make_intrusive<detail::CancelationState>();
        // Make the state uncancelable.
        state->dismiss();
        return CancelationToken(std::move(state));
    }

    explicit CancelationToken(boost::intrusive_ptr<const detail::CancelationState> state)
        : _state(std::move(state)) {}

    ~CancelationToken() = default;

    CancelationToken(const CancelationToken& other) = default;
    CancelationToken& operator=(const CancelationToken& other) = default;

    CancelationToken(CancelationToken&& other) = default;
    CancelationToken& operator=(CancelationToken&& other) = default;

    /**
     * Returns whether or not cancel() has been called on the CancelationSource object from which
     * this token was constructed.
     */
    bool isCanceled() const {
        return _state->isCanceled();
    }

    /**
     * Returns a future that will be resolved with success when cancel() has been called on the
     * CancelationSource object from which this token was constructed, or with an error containing
     * CallbackCanceled if that CancelationSource object is destroyed without having cancel() called
     * on it.
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
     * Points to the object containing the status of cancelation.
     */
    boost::intrusive_ptr<const detail::CancelationState> _state;
};

/**
 * Type used to manage the cancelation of a task. CancelationSource is used to cancel a task, and
 * CancelationTokens obtained via CancelationSource::token() are used to check for and handle
 * cancelation.
 */
class CancelationSource {
public:
    CancelationSource() = default;
    /**
     * Creates a CancelationSource that will be canceled when the input token is canceled. This
     * allows the construction of cancelation hierarchies.
     *
     * For example, if we have:
     *
     * CancelationSource first;
     * CancelationSource second(first.token());
     * CancelationSource third(second.token());
     *
     * Calling third.cancel() will only cancel tokens obtained from third.
     * Calling second.cancel() will cancel tokens obtained from second, and call third.cancel().
     * Calling first.cancel() will thus cancel the whole hierarchy.
     */
    explicit CancelationSource(const CancelationToken& token) {
        // Cancel the source when the input token is canceled.
        //
        // Note that because this captures the CancelationState object directly, and not the
        // CancelationStateHolder, this will still allow callback state attached to this
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
     * cancelation callbacks.
     */
    ~CancelationSource() = default;

    CancelationSource(const CancelationSource& other) = default;
    CancelationSource& operator=(const CancelationSource& other) = default;

    CancelationSource(CancelationSource&& other) = default;
    CancelationSource& operator=(CancelationSource&& other) = default;

    /**
     * Cancel the token. If no call to cancel has previously been made, this will cause all
     * callbacks chained to this token via the onCancel future to be run. Otherwise, does nothing.
     */
    void cancel() {
        _stateHolder->get()->cancel();
    }

    /**
     * Returns a CancelationToken which will be canceled when this source is
     * canceled.
     */
    CancelationToken token() const {
        return CancelationToken{_stateHolder->get()};
    }

private:
    boost::intrusive_ptr<detail::CancelationStateHolder> _stateHolder{
        make_intrusive<detail::CancelationStateHolder>()};
};

}  // namespace mongo
