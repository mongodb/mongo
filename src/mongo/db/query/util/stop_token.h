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

#include <memory>

namespace mongo {
// Shared state held by one stop_source, and N stop_tokens.
struct stop_state {
    mongo::Atomic<bool> stop = false;
};


class stop_token;

/**
 * Used to inform N tasks/threads (via stop_tokens) that they are requested to stop.
 */
class stop_source {
public:
    stop_token get_token() const;

    void request_stop() {
        _state->stop.store(true);
    }

    ~stop_source() {
        request_stop();
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
        return _state && _state->stop.load();
    }

    bool stop_possible() const {
        return bool(_state);
    }

private:
    std::shared_ptr<stop_state> _state = nullptr;
};

inline stop_token stop_source::get_token() const {
    return {_state};
}
}  // namespace mongo
