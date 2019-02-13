/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <arm_neon.h>
#include <cstdint>

#include "mongo/platform/bits.h"

namespace mongo {
namespace unicode {

/**
 * A sequence of bytes that can be manipulated using vectorized instructions.
 *
 * This is specific to the use case in mongo::unicode::String and not intended as a general purpose
 * vector class.
 *
 * This specialization offers acceleration for aarch64.
 */
class ByteVector {
public:
    using Native = uint8x16_t;
    using Mask = uint16_t;
    using Scalar = int8_t;
    static const int size = sizeof(Native);

    /**
     * Sets all bytes to 0.
     */
    ByteVector() : _data(vdupq_n_u8(0)) {}

    /**
     * Sets all bytes to val.
     */
    explicit ByteVector(Scalar val) : _data(vdupq_n_u8(val)) {}

    /**
     * Load a vector from a potentially unaligned location.
     */
    static ByteVector load(const void* ptr) {
        // This function is documented as taking an unaligned pointer.
        return vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
    }

    /**
     * Store this vector to a potentially unaligned location.
     */
    void store(void* ptr) const {
        // This function is documented as taking an unaligned pointer.
        vst1q_u8(reinterpret_cast<uint8_t*>(ptr), _data);
    }

    /**
     * Returns a bitmask with the high bit from each byte.
     */
    Mask maskHigh() const {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
        uint64x1_t p;
        // vset_lane_u64 initializes p but the compiler does not understand this and considers p
        // uninitialized.
        p = vset_lane_u64(0x8040201008040201, p, 0);
#pragma GCC diagnostic pop
        uint8x16_t powers = vcombine_u8(vreinterpret_u8_u64(p), vreinterpret_u8_u64(p));
        int8x16_t zero8x16 = vdupq_n_s8(0);
        uint8x16_t input = vcltq_s8(vreinterpretq_s8_u8(_data), zero8x16);
        uint64x2_t mask = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(vandq_u8(input, powers))));
        uint16_t output;
        output = ((vgetq_lane_u8(vreinterpretq_u8_u64(mask), 8) << 8) |
                  (vgetq_lane_u8(vreinterpretq_u8_u64(mask), 0) << 0));
        return output;
    }

    /**
     * Returns a bitmask with any bit from each byte.
     *
     * This operation only makes sense if all bytes are either 0x00 or 0xff, such as the result from
     * comparison operations.
     */
    Mask maskAny() const {
        return maskHigh();  // Other archs may be more efficient here.
    }

    /**
     * Counts zero bits in mask from whichever side corresponds to the lowest memory address.
     */
    static uint32_t countInitialZeros(Mask mask) {
        return mask == 0 ? size : countTrailingZeros64(mask);
    }

    /**
     * Sets each byte to 0xff if it is ==(EQ), <(LT), or >(GT), otherwise 0x00.
     *
     * May use either signed or unsigned comparisons since this use case doesn't care about bytes
     * with high bit set.
     */
    ByteVector compareEQ(Scalar val) const {
        return vceqq_u8(_data, ByteVector(val)._data);
    }
    ByteVector compareLT(Scalar val) const {
        return vcltq_u8(_data, ByteVector(val)._data);
    }
    ByteVector compareGT(Scalar val) const {
        return vcgtq_u8(_data, ByteVector(val)._data);
    }

    ByteVector operator|(ByteVector other) const {
        return vorrq_u8(_data, other._data);
    }

    ByteVector& operator|=(ByteVector other) {
        return (*this = (*this | other));
    }

    ByteVector operator&(ByteVector other) const {
        return vandq_u8(_data, other._data);
    }

    ByteVector& operator&=(ByteVector other) {
        return (*this = (*this & other));
    }

private:
    ByteVector(Native data) : _data(data) {}

    Native _data;
};

}  // namespace unicode
}  // namespace mongo
