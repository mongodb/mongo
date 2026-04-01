/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo::transaction_participant_details {

/**
 * Wrapper for fields that are not reconstructed when a prepared transaction is recovered from
 * a precise checkpoint. This makes it explicit at the call site whether the caller expects the
 * field to be available or is knowingly accessing it in a possible post recovery case.
 */
template <typename T>
class NonRecoverableField {
public:
    NonRecoverableField() = default;
    NonRecoverableField(const NonRecoverableField&) = delete;
    NonRecoverableField& operator=(const NonRecoverableField&) = delete;
    NonRecoverableField(NonRecoverableField&&) noexcept = delete;
    NonRecoverableField& operator=(NonRecoverableField&&) noexcept = delete;

    /**
     * Returns a reference to the wrapped value. Invariants that the field is available, i.e. the
     * transaction has not been through prepared transaction recovery from a precise checkpoint.
     * Use in code paths that should never run after recovery.
     */
    T& getExpectAvailable() {
        tassert(11742200,
                "Field is not available for prepared transactions recovered from a "
                "precise checkpoint",
                _available);
        return _value;
    }
    const T& getExpectAvailable() const {
        tassert(11742201,
                "Field is not available for prepared transactions recovered from a "
                "precise checkpoint",
                _available);
        return _value;
    }

    /**
     * Returns a reference to the wrapped value without checking availability. Use in code
     * paths that may run after prepared transaction recovery from a precise checkpoint (e.g.,
     * clearing on reset, reading metrics from a possibly-empty field).
     */
    T& getAllowUnavailable() {
        return _value;
    }
    const T& getAllowUnavailable() const {
        return _value;
    }

    /**
     * Marks the field as unavailable, indicating the transaction was recovered from a precise
     * checkpoint and this field was not reconstructed.
     */
    void markAsUnavailable() {
        _available = false;
    }

    /**
     * Sets the field to the given value and marks it as available.
     */
    void setAvailable(T&& value) {
        _value = std::move(value);
        _available = true;
    }

    /**
     * Resets the field to its default value and marks it as available again.
     */
    void reset() {
        _value = T{};
        _available = true;
    }

private:
    T _value{};
    bool _available{true};
};

}  // namespace mongo::transaction_participant_details
