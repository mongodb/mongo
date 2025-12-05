/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

/**
 * Shim utilities for std::stop_source, which is not yet available in all
 * compiler versions in use.
 *
 * Approximates the interface of std::stop_source, std::stop_token.
 */
#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/list_hook.hpp>

namespace mongo {

namespace detail {
class stop_callback_base {
public:
    virtual void invoke() = 0;
    boost::intrusive::list_member_hook<> _member_hook;
};

/**
 * Intrusive linked list of mongo::stop_callback instances.
 * Non-owning - stop_callbacks are constructed on the stack, and
 * linked/unlinked into a callback list for the corresponding stop_state at
 * construction/destruction.
 */
using CallbackList = boost::intrusive::list<
    detail::stop_callback_base,
    boost::intrusive::member_hook<detail::stop_callback_base,
                                  boost::intrusive::list_member_hook<>,
                                  &detail::stop_callback_base::_member_hook>>;
}  // namespace detail

// Shared state held by one stop_source, N stop_tokens, N stop_callbacks.
class stop_state {
public:
    void stop() {
        if (_stop.load()) {
            // If stop has already been triggered, all registered callbacks
            // have already been invoked.
            return;
        }
        std::unique_lock ul{_mutex};
        _stop.store(true);
        for (auto& callback : _callbacks) {
            callback.invoke();
        }
    }

    bool stopped() const {
        return _stop.load();
    }

private:
    bool addCallback(detail::stop_callback_base& cb) {
        std::unique_lock ul{_mutex};
        if (_stop.load()) {
            // Stop already requested; the callback should be invoked inline.
            return false;
        }
        _callbacks.push_back(cb);
        return true;
    }

    void removeCallback(detail::stop_callback_base& cb) {
        std::unique_lock ul{_mutex};
        if (!cb._member_hook.is_linked()) {
            return;
        }
        _callbacks.erase(_callbacks.iterator_to(cb));
    }

    std::mutex _mutex;
    mongo::Atomic<bool> _stop = false;
    // List of all currently registered callbacks. Non-owning.
    detail::CallbackList _callbacks;

    template <class Callback>
    friend class stop_callback;
};


class stop_token;

/**
 * Used to inform N tasks/threads (via stop_tokens) that they are requested to stop.
 */
class stop_source {
public:
    stop_token get_token() const;

    void request_stop() {
        _state->stop();
    }

private:
    std::shared_ptr<stop_state> _state = std::make_shared<stop_state>();
};

/**
 * Used by a task/thread to periodically check if it has been requested to stop.
 *
 * Created from a stop_source with get_token().
 */
class stop_token {
public:
    stop_token() = default;
    stop_token(std::shared_ptr<stop_state> state) : _state(std::move(state)) {}
    bool stop_requested() const {
        return _state && _state->stopped();
    }

    bool stop_possible() const {
        return bool(_state);
    }

private:
    std::shared_ptr<stop_state> _state = nullptr;
    template <class Callback>
    friend class stop_callback;
};

inline stop_token stop_source::get_token() const {
    return {_state};
}


template <class Callback>
class stop_callback final : private detail::stop_callback_base {
public:
    stop_callback(stop_token token, Callback callback)
        : _cb(std::move(callback)), _state(token._state) {
        if (_state && !_state->addCallback(*this)) {
            // Stop was possible but was already requested, callback not enqueued.
            // Immediately run the callback.
            _cb();
        }
    }
    ~stop_callback() noexcept {
        // Safe to call even if not successfully added previously.
        if (_state) {
            _state->removeCallback(*this);
        }
    }

private:
    void invoke() final {
        _cb();
    }

    MONGO_COMPILER_NO_UNIQUE_ADDRESS Callback _cb;
    std::shared_ptr<stop_state> _state;
};
}  // namespace mongo
