// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * FLE2 Range Utility functions
 */

/**
 * Describe the encoding of an BSON int32
 *
 * NOTE: It is not a mistake that a int32 is encoded as uint32.
 */
struct OSTType_Int32 {
    OSTType_Int32(uint32_t v, uint32_t minP, uint32_t maxP) : value(v), min(minP), max(maxP) {}

    uint32_t value;
    uint32_t min;
    uint32_t max;
};

OSTType_Int32 getTypeInfo32(int32_t value,
                            boost::optional<int32_t> min,
                            boost::optional<int32_t> max);

/**
 * Describe the encoding of an BSON int64
 *
 * NOTE: It is not a mistake that a int64 is encoded as uint64.
 */
struct OSTType_Int64 {
    OSTType_Int64(uint64_t v, uint64_t minP, uint64_t maxP) : value(v), min(minP), max(maxP) {}

    uint64_t value;
    uint64_t min;
    uint64_t max;
};

OSTType_Int64 getTypeInfo64(int64_t value,
                            boost::optional<int64_t> min,
                            boost::optional<int64_t> max);


/**
 * Describe the encoding of an BSON double (i.e. IEEE 754 Binary64)
 *
 * NOTE: It is not a mistake that a double is encoded as uint64.
 */
struct OSTType_Double {
    OSTType_Double(uint64_t v, uint64_t minP, uint64_t maxP) : value(v), min(minP), max(maxP) {}

    uint64_t value;
    uint64_t min;
    uint64_t max;
};

OSTType_Double getTypeInfoDouble(double value,
                                 boost::optional<double> min,
                                 boost::optional<double> max,
                                 boost::optional<uint32_t> precision);
/**
 * Describe the encoding of an BSON Decimal (i.e. IEEE 754 Decimal128)
 *
 * NOTE: It is not a mistake that a decimal is encoded as uint128.
 */

struct OSTType_Decimal128 {
    OSTType_Decimal128(boost::multiprecision::uint128_t v,
                       boost::multiprecision::uint128_t minP,
                       boost::multiprecision::uint128_t maxP)
        : value(v), min(minP), max(maxP) {}

    boost::multiprecision::uint128_t value;
    boost::multiprecision::uint128_t min;
    boost::multiprecision::uint128_t max;
};

boost::multiprecision::uint128_t toUInt128FromDecimal128(Decimal128 dec);

OSTType_Decimal128 getTypeInfoDecimal128(Decimal128 value,
                                         boost::optional<Decimal128> min,
                                         boost::optional<Decimal128> max,
                                         boost::optional<uint32_t> precision);

/**
 * Determines whether the range of Decimal128 values, between min and max (inclusive),
 * with the given precision, can be re-encoded in less than 128 bits, in order to reduce
 * the size of the hypergraph (ie. "precision-mode"). If so, the number of bits needed
 * to represent the range of values is returned in maxBitsOut.
 */
bool canUsePrecisionMode(Decimal128 min,
                         Decimal128 max,
                         uint32_t precision,
                         uint32_t* maxBitsOut = nullptr);
/**
 * Determines whether the range of double-precision floating point values, between min and max
 * (inclusive), with the given precision, can be re-encoded in less than 64 bits, in order to
 * reduce the size of the hypergraph (ie. "precision-mode"). If so, the number of bits needed to
 * represent the range of values is returned in maxBitsOut.
 */
bool canUsePrecisionMode(double min,
                         double max,
                         uint32_t precision,
                         uint32_t* maxBitsOut = nullptr);

/**
 * Return the first bit set in a integer. 1 indexed.
 */
template <typename T>
inline int getFirstBitSet(T v) {
    return 64 - countLeadingZeros64(v);
}

template <>
inline int getFirstBitSet<boost::multiprecision::uint128_t>(
    const boost::multiprecision::uint128_t v) {
    return boost::multiprecision::msb(v) + 1;
}

template <>
inline int getFirstBitSet<boost::multiprecision::int128_t>(
    const boost::multiprecision::int128_t v) {
    return boost::multiprecision::msb(v) + 1;
}

}  // namespace mongo
