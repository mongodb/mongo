// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

/**
 * Common utilities used across different implementations
 * (document / range bounds & removable / non-removable windows) of $minMaxScaler window function.
 */

namespace mongo {
namespace min_max_scaler {
/**
 * Represents a pairing of min and max values;
 * a common concept used in $minMaxScaler.
 *
 * Non-const instantiations of this object support updates to min and max
 * (i.e. for tracking the min and max seen in a sliding window).
 * Const instantiations of this object only store and retrieve a pre-set min and max
 * (i.e. for tracking the domain min and max args in $minMaxScaler).
 *
 * Does not support removals of previously updated / provided values,
 * and as such cannot be used to track the min and max of a sliding window that removes values.
 */
class MinAndMax {
public:
    MinAndMax() {}

    // Construct with preset min and max.
    MinAndMax(const Value& min, const Value& max) : _min(min), _max(max), _noValuesSeen(false) {
        tassert(
            9522903,
            "min_max_scaler::MinAndMax must be provided with numeric Value types upon construction",
            min.numeric() && max.numeric());
        tassert(9522900,
                "The min cannot be greater than max when constructing "
                "min_max_scaler::MinAndMax. "
                "min = '" +
                    min.toString() + "', max = " + max.toString() + "'.",
                Value::compare(min, max, nullptr) <= 0);
    }

    const Value& min() const {
        tassert(9522901,
                "min_max_scaler::MinAndMax::min() was requested on a empty"
                "(non-removable / left-unbounded) window during a $minMaxScaler computation.",
                !_noValuesSeen);
        return _min;
    }

    const Value& max() const {
        tassert(9522902,
                "min_max_scaler::MinAndMax::max() was requested on a empty"
                "(non-removable / left-unbounded) window during a $minMaxScaler computation.",
                !_noValuesSeen);
        return _max;
    }

    // Potentially update the min and max with a newly seen value.
    void update(const Value& value) {
        tassert(9522904,
                "min_max_scaler::update must be provided with a numeric Value type.",
                value.numeric());
        if (_noValuesSeen) {
            // First document in the window.
            _min = value;
            _max = value;
            _noValuesSeen = false;
        } else {
            // No collation needs to be specified here,
            // as '_min', '_max' and 'value' are always numeric.
            if (Value::compare(_min, value, nullptr) > 0) {
                // New window min found.
                _min = value;
            }
            if (Value::compare(_max, value, nullptr) < 0) {
                // New window max found.
                _max = value;
            }
        }
    }

    // Reset to no values seen; thus min and max become reset and invalid.
    void reset() {
        _min = Value();
        _max = Value();
        _noValuesSeen = true;
    }

private:
    // The min and max values of the window being tracked.
    // Note we only track these two values, and not every value in the window,
    // and thus removing from the window is (intentionally) not supported.
    Value _min;
    Value _max;

    // Keeps track no values have ever been seen, and thus the min and max are invalid.
    bool _noValuesSeen = true;
};

/**
 * Computes the result of $minMaxScaler operation, provided all needed inputs.
 *
 * Output = (((currentValue - windowMin) / (windowMax - windowMin)) * (sMax - sMin)) + sMin
 *
 * Output is always bounded between sMin and sMax, inclusive.
 */
Value computeResult(const Value& currentValue,
                    const MinAndMax& windowMinAndMax,
                    const MinAndMax& sMinAndMax);

};  // namespace min_max_scaler
};  // namespace mongo
