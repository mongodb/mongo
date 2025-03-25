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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/assert_util.h"

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
