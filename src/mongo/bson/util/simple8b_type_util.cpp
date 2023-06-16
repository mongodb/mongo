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

#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cmath>
#include <cstring>

#include <absl/numeric/int128.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonelement.h"

namespace mongo {
namespace {
int128_t encodeCharArray(const char (&arr)[16]) {
    uint64_t low = ConstDataView(arr).read<LittleEndian<uint64_t>>();
    uint64_t high = ConstDataView(arr + 8).read<LittleEndian<uint64_t>>();
    return absl::MakeInt128(high, low);
}
}  // namespace

int64_t Simple8bTypeUtil::encodeObjectId(const OID& oid) {
    uint64_t encoded = 0;
    uint8_t* encodedBytes = reinterpret_cast<uint8_t*>(&encoded);

    ConstDataView cdv = oid.view();

    // Copy counter and timestamp bytes so that they match the specs in the header.
    encodedBytes[0] = cdv.read<uint8_t>(11);  // Counter index 2.
    encodedBytes[1] = cdv.read<uint8_t>(3);   // Timestamp index 3.
    encodedBytes[2] = cdv.read<uint8_t>(10);  // Counter index 1.
    encodedBytes[3] = cdv.read<uint8_t>(2);   // Timestamp index 2.
    encodedBytes[4] = cdv.read<uint8_t>(9);   // Counter index 0.
    encodedBytes[5] = cdv.read<uint8_t>(1);   // Timestamp index 1.
    encodedBytes[6] = cdv.read<uint8_t>(0);   // Timestamp index 0.

    return LittleEndian<uint64_t>::load(encoded);
}

void Simple8bTypeUtil::decodeObjectIdInto(char* buffer,
                                          int64_t val,
                                          OID::InstanceUnique processUnique) {
    val = LittleEndian<uint64_t>::store(val);
    uint8_t* encodedBytes = reinterpret_cast<uint8_t*>(&val);

    // Set Timestamp and Counter variables together.
    buffer[0] = encodedBytes[6];   // Timestamp index 0.
    buffer[1] = encodedBytes[5];   // Timestamp index 1.
    buffer[2] = encodedBytes[3];   // Timestamp index 2.
    buffer[3] = encodedBytes[1];   // Timestamp index 3.
    buffer[9] = encodedBytes[4];   // Counter index 0;
    buffer[10] = encodedBytes[2];  // Counter index 1.
    buffer[11] = encodedBytes[0];  // Counter index 2.

    // Finally set Process Unique.
    std::copy(processUnique.bytes,
              processUnique.bytes + OID::kInstanceUniqueSize,
              buffer + OID::kTimestampSize);
}

OID Simple8bTypeUtil::decodeObjectId(int64_t val, OID::InstanceUnique processUnique) {
    unsigned char objId[OID::kOIDSize];
    decodeObjectIdInto(reinterpret_cast<char*>(objId), val, processUnique);
    return OID(objId);
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

boost::optional<int64_t> Simple8bTypeUtil::encodeDouble(double val, uint8_t scaleIndex) {
    auto bitCastToInt64 = [](double value) {
        int64_t ret;
        memcpy(&ret, &value, sizeof(ret));
        return ret;
    };

    if (scaleIndex == kMemoryAsInteger) {
        return bitCastToInt64(val);
    }

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

    // We need to check that we can get the exact bit pattern back. 0.0 and -0.0 compares as equal
    // when comparing as doubles but they have different bit patterns.
    if (bitCastToInt64(valueToBeEncoded / scaleMultiplier) != bitCastToInt64(val)) {
        return boost::none;
    }
    // Delta encoding. Gap should never induce overflow
    return valueToBeEncoded;
}

double Simple8bTypeUtil::decodeDouble(int64_t val, uint8_t scaleIndex) {
    if (scaleIndex == kMemoryAsInteger) {
        double ret;
        memcpy(&ret, &val, sizeof(ret));
        return ret;
    }

    return val / kScaleMultiplier[scaleIndex];
}

int128_t Simple8bTypeUtil::encodeDecimal128(Decimal128 val) {
    Decimal128::Value underlyingData = val.getValue();
    return absl::MakeInt128(underlyingData.high64, underlyingData.low64);
}

Decimal128 Simple8bTypeUtil::decodeDecimal128(int128_t val) {
    Decimal128::Value constructFromValue;
    constructFromValue.high64 = absl::Uint128High64(val);
    constructFromValue.low64 = absl::Uint128Low64(val);
    Decimal128 res(constructFromValue);
    return res;
}

boost::optional<int128_t> Simple8bTypeUtil::encodeBinary(const char* val, size_t size) {
    if (size > 16)
        return boost::none;

    char arr[16] = {};
    memcpy(arr, val, size);
    return encodeCharArray(arr);
}

void Simple8bTypeUtil::decodeBinary(int128_t val, char* result, size_t size) {
    uint64_t low = LittleEndian<uint64_t>::store(absl::Int128Low64(val));
    uint64_t high = LittleEndian<uint64_t>::store(absl::Int128High64(val));
    if (size > 8) {
        memcpy(result, &low, 8);
        memcpy(result + 8, &high, size - 8);
    } else {
        memcpy(result, &low, size);
    }
}

boost::optional<int128_t> Simple8bTypeUtil::encodeString(StringData str) {
    auto size = str.size();
    if (size > 16)
        return boost::none;

    // Strings are reversed as it is deemed likely that entopy is located at the end of the string.
    // This will put the entropy in the least significant byte creating a smaller delta. We can't
    // have leading zero bytes as that would create a decoding ambiguity. Empty strings are fine
    // however, they are just encoded as 0.
    if (!str.empty() && str[0] == '\0')
        return boost::none;

    char arr[16] = {};
    std::reverse_copy(str.begin(), str.end(), arr);
    return encodeCharArray(arr);
}
Simple8bTypeUtil::SmallString Simple8bTypeUtil::decodeString(int128_t val) {
    // String may be up to 16 characters, provide that decode and then we need to scan the result to
    // find actual size
    char str[16] = {};
    decodeBinary(val, str, 16);

    // Find first non null character from the end of the string to determine actual size
    int8_t i = 15;
    for (; i >= 0 && str[i] == '\0'; --i) {
    }

    // Reverse and return string
    SmallString ret;
    ret.size = i + 1;
    std::reverse_copy(str, str + ret.size, ret.str.data());
    return ret;
}

}  // namespace mongo
