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

#include <absl/base/config.h>
#include <absl/numeric/int128.h>
#include <array>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/int128.h"

namespace mongo {

/*
 * TypeCompressor: This class allows storing a wide variety of types using a series of compression
 * methods. Each supported type has an overloaded encode and decode method and a short comment
 * explaining the type of current compression used
 */
class Simple8bTypeUtil {
public:
    // These methods are for encoding a signed integer with simple8b. They move the signed bit from
    // the most significant bit position to the least significant bit to provide the ability to
    // store as an unsigned integer
    // the most significant bit position to the least significant bit and call simple8b as an
    // unsigned integer.
    static uint64_t encodeInt64(int64_t val) {
        return (static_cast<uint64_t>(val) << 1) ^ (val >> 63);
    }
    static int64_t decodeInt64(uint64_t val) {
        return (val >> 1) ^ (~(val & 1) + 1);
    }
    static uint128_t encodeInt128(int128_t val) {
        // The Abseil right shift implementation on signed int128 is not correct as an arithmetic
        // shift in their non-intrinsic implementation. When we detect this case we replace the
        // right arithmetic shift of 127 positions that needs to produce 0xFF..FF or 0x00..00
        // depending on the sign bit. We take the high 64 bits and performing a right arithmetic
        // shift 63 positions which produces 0xFF..FF if the sign bit is set and 0x00..00 otherwise.
        // We can then use this value in both the high and low components of int128 to produce the
        // value that we need.
#if defined(ABSL_HAVE_INTRINSIC_INT128)
        return (static_cast<uint128_t>(val) << 1) ^ (val >> 127);
#else
        // get signed bit
        uint64_t component = absl::Int128High64(val) >> 63;
        return (static_cast<uint128_t>(val) << 1) ^ absl::MakeUint128(component, component);
#endif
    }

    static int128_t decodeInt128(uint128_t val) {
        return static_cast<int128_t>((val >> 1) ^ (~(val & 1) + 1));
    }

    // These methods are for encoding OID with simple8b. The unique identifier is not part of
    // the encoded integer and must thus be provided when decoding.
    // Re-organize the bytes so that most of the entropy is in the least significant bytes.
    // Since TS = Timestamp is in big endian and C = Counter is in big endian,
    // then rearrange the bytes to:
    // | Byte Usage | C2 | TS3 | C1 | TS2 | C0 | TS1 | TS0 |
    // | Byte Index |  0 |  1  |  2 |  3  |  4 |  5  |  6  |
    // The buffer passed to decodeObjectIdInto() must have at least OID::kOIDSize size.
    static int64_t encodeObjectId(const OID& oid);
    static void decodeObjectIdInto(char* buffer, int64_t val, OID::InstanceUnique processUnique);
    static OID decodeObjectId(int64_t val, OID::InstanceUnique processUnique);

    // These methods add floating point support for encoding and decoding with simple8b up to 8
    // decimal digits. They work by multiplying the floating point value by a multiple of 10 and
    // rounding to the nearest integer. They return a option that will not be valid in the case of a
    // value being greater than 8 decimal digits. Additionally, they will return a boost::none in
    // the cae that compression is not feasible.
    static boost::optional<uint8_t> calculateDecimalShiftMultiplier(double val);
    static boost::optional<int64_t> encodeDouble(double val, uint8_t scaleIndex);
    static double decodeDouble(int64_t val, uint8_t scaleIndex);

    // These methods allow encoding decimal 128 with simple8b.
    static int128_t encodeDecimal128(Decimal128 val);
    static Decimal128 decodeDecimal128(int128_t val);

    // These methods allow encoding binary with simple8b. We do not make any
    // assumptions about the data other than the fact that the data is valid up to the size
    // provided. Encoding is only possible for sizes less than or equal to 16 bytes. boost::none is
    // returned if encoding is not possible.
    static boost::optional<int128_t> encodeBinary(const char* val, size_t size);
    static void decodeBinary(int128_t val, char* result, size_t size);

    // These methods allow encoding strings with simple8b. Encoding is only possible for strings
    // less than or equal to 16 bytes and for strings starting with a non-null character.
    // boost::none is returned if encoding is not possible.
    struct SmallString {
        std::array<char, 16> str;
        uint8_t size;
    };
    static boost::optional<int128_t> encodeString(StringData str);
    static SmallString decodeString(int128_t val);

    // Array is a double as it will always be multiplied by a double and we don't want to do an
    // extra cast for
    static constexpr uint8_t kMemoryAsInteger = 5;
    static constexpr std::array<double, kMemoryAsInteger> kScaleMultiplier = {
        1, 10, 100, 10000, 100000000};
};
}  // namespace mongo
