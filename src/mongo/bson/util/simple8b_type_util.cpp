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

#include "mongo/base/data_type_endian.h"
#include "mongo/bson/bsonelement.h"

#include <cmath>

namespace mongo {

uint64_t Simple8bTypeUtil::encodeInt64(int64_t val) {
    return (static_cast<uint64_t>(val) << 1) ^ (val >> 63);
}

int64_t Simple8bTypeUtil::decodeInt64(uint64_t val) {
    return (val >> 1) ^ (~(val & 1) + 1);
}

uint128_t Simple8bTypeUtil::encodeInt128(int128_t val) {
// The Abseil right shift implementation on signed int128 is not correct as an arithmetic shift in
// their non-intrinsic implementation. When we detect this case we replace the right arithmetic
// shift of 127 positions that needs to produce 0xFF..FF or 0x00..00 depending on the sign bit. We
// take the high 64 bits and performing a right arithmetic shift 63 positions which produces
// 0xFF..FF if the sign bit is set and 0x00..00 otherwise. We can then use this value in both the
// high and low components of int128 to produce the value that we need.
#if defined(ABSL_HAVE_INTRINSIC_INT128)
    return (static_cast<uint128_t>(val) << 1) ^ (val >> 127);
#else
    // get signed bit
    uint64_t component = absl::Int128High64(val) >> 63;
    return (static_cast<uint128_t>(val) << 1) ^ absl::MakeUint128(component, component);
#endif
}

int128_t Simple8bTypeUtil::decodeInt128(uint128_t val) {
    return static_cast<int128_t>((val >> 1) ^ (~(val & 1) + 1));
}

int64_t Simple8bTypeUtil::encodeObjectId(const OID& oid) {
    uint64_t encoded = 0;
    uint8_t* encodedBytes = reinterpret_cast<uint8_t*>(&encoded);

    ConstDataView cdv = oid.view();

    // Copy counter and timestamp bytes so that they match the specs in the header.
    encodedBytes[0] = cdv.read<uint8_t>(3);   // Timestamp index 3.
    encodedBytes[1] = cdv.read<uint8_t>(11);  // Counter index 2.
    encodedBytes[2] = cdv.read<uint8_t>(2);   // Timestamp index 2.
    encodedBytes[3] = cdv.read<uint8_t>(10);  // Counter index 1.
    encodedBytes[4] = cdv.read<uint8_t>(1);   // Timestamp index 1.
    encodedBytes[5] = cdv.read<uint8_t>(9);   // Counter index 0.
    encodedBytes[6] = cdv.read<uint8_t>(0);   // Timestamp index 0.

    return LittleEndian<uint64_t>::load(encoded);
}

OID Simple8bTypeUtil::decodeObjectId(int64_t val, OID::InstanceUnique processUnique) {
    unsigned char objId[OID::kOIDSize];

    val = LittleEndian<uint64_t>::store(val);
    uint8_t* encodedBytes = reinterpret_cast<uint8_t*>(&val);

    // Set Timestamp and Counter variables together.
    objId[0] = encodedBytes[6];   // Timestamp index 0.
    objId[1] = encodedBytes[4];   // Timestamp index 1.
    objId[2] = encodedBytes[2];   // Timestamp index 2.
    objId[3] = encodedBytes[0];   // Timestamp index 3.
    objId[9] = encodedBytes[5];   // Counter index 0;
    objId[10] = encodedBytes[3];  // Counter index 1.
    objId[11] = encodedBytes[1];  // Counter index 2.

    // Finally set Process Unique.
    std::copy(processUnique.bytes,
              processUnique.bytes + OID::kInstanceUniqueSize,
              objId + OID::kTimestampSize);

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
    if (scaleIndex == kMemoryAsInteger)
        return *reinterpret_cast<int64_t*>(&val);

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
    // Delta encoding. Gap should never induce overflow
    return valueToBeEncoded;
}

double Simple8bTypeUtil::decodeDouble(int64_t val, uint8_t scaleIndex) {
    if (scaleIndex == kMemoryAsInteger)
        return *reinterpret_cast<double*>(&val);

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

}  // namespace mongo
