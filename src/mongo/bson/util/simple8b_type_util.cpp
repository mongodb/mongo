/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/util/simple8b_type_util.h"
#include "mongo/bson/bsonelement.h"

#include <cmath>

namespace mongo {

uint64_t Simple8bTypeUtil::encodeInt64(int64_t val) {
    return (uint64_t(val) << 1) ^ (val >> 63);
}

int64_t Simple8bTypeUtil::decodeInt64(uint64_t val) {
    return (val >> 1) ^ (~(val & 1) + 1);
}

boost::optional<uint8_t> Simple8bTypeUtil::calculateDecimalShiftMultiplier(double val) {
    // Don't store isnan and isinf and end calculation early
    if (std::isnan(val) || std::isinf(val)) {
        return boost::none;
    }
    // Try multiplying by selected powers of 10 until we do not have any decimal digits. If we
    // always have leftover digits, return none.
    for (uint8_t i = 0; i < kScaleMultiplier.size(); ++i) {
        double scaleMultiplier = kScaleMultiplier[i];
        double valTimesMultiplier = val * scaleMultiplier;
        // Checks for both overflows
        // We use 2^53 because this is the max integer that we can guarentee can be
        // exactly represented by a floating point decimal since there are 53 value bits
        // in a IEEE754 Floating 64 representation
        if (!(valTimesMultiplier >= BSONElement::kSmallestSafeLongLongAsDouble        // -2^53
              && valTimesMultiplier <= BSONElement::kLargestSafeLongLongAsDouble)) {  // 2^53
            return boost::none;
        }
        int64_t decimalToBeEncoded = std::llround(valTimesMultiplier);
        if (decimalToBeEncoded / scaleMultiplier == val) {
            return i;
        }
    }
    return boost::none;
}

boost::optional<uint64_t> Simple8bTypeUtil::encodeDouble(double val, uint8_t scaleIndex) {
    // Checks for both overflow and handles NaNs
    // We use 2^53 because this is the max integer that we can guarentee can be
    // exactly represented by a floating point decimal since there are 53 value bits
    // in a IEEE754 Floating 64 representation
    double scaleMultiplier = kScaleMultiplier[scaleIndex];
    double valTimesMultiplier = val * scaleMultiplier;
    if (!(valTimesMultiplier >= BSONElement::kSmallestSafeLongLongAsDouble     // -2^53
          && valTimesMultiplier <= BSONElement::kLargestSafeLongLongAsDouble)  // 2^53
    ) {
        return boost::none;
    }

    // Check to make sure our exact encoded value can be exactly converted back into a decimal.
    // We use encodeInt64 to handle negative floats by taking the signed bit and placing it at the
    // lsb position
    int64_t valueToBeEncoded = std::llround(valTimesMultiplier);
    if (valueToBeEncoded / scaleMultiplier != val) {
        return boost::none;
    }
    return encodeInt64(valueToBeEncoded);
}

double Simple8bTypeUtil::decodeDouble(uint64_t val, uint8_t scaleIndex) {
    return double(decodeInt64(val)) / kScaleMultiplier[scaleIndex];
}

}  // namespace mongo
