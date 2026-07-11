// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
