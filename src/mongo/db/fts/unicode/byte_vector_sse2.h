// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/bits.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <emmintrin.h>

namespace mongo {
namespace unicode {

/**
 * A sequence of bytes that can be manipulated using vectorized instructions.
 *
 * This is specific to the use case in mongo::unicode::String and not intended as a general purpose
 * vector class.
 *
 * This specialization offers acceleration for x86_64
 */
class ByteVector {
public:
    using Native = __m128i;
    using Mask = uint32_t;  // should be uint16_t but better codegen with uint32_t.
    using Scalar = int8_t;
    static const int size = sizeof(Native);

    /**
     * Sets all bytes to 0.
     */
    ByteVector() : _data(_mm_setzero_si128()) {}

    /**
     * Sets all bytes to val.
     */
    explicit ByteVector(Scalar val) : _data(_mm_set1_epi8(val)) {}

    /**
     * Load a vector from a potentially unaligned location.
     */
    static ByteVector load(const void* ptr) {
        // This function is documented as taking an unaligned pointer.
        return _mm_loadu_si128(reinterpret_cast<const Native*>(ptr));
    }

    /**
     * Store this vector to a potentially unaligned location.
     */
    void store(void* ptr) const {
        // This function is documented as taking an unaligned pointer.
        _mm_storeu_si128(reinterpret_cast<Native*>(ptr), _data);
    }

    /**
     * Returns a bitmask with the high bit from each byte.
     */
    Mask maskHigh() const {
        return _mm_movemask_epi8(_data);
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
        return _mm_cmpeq_epi8(_data, ByteVector(val)._data);
    }
    ByteVector compareLT(Scalar val) const {
        return _mm_cmplt_epi8(_data, ByteVector(val)._data);
    }
    ByteVector compareGT(Scalar val) const {
        return _mm_cmpgt_epi8(_data, ByteVector(val)._data);
    }

    ByteVector operator|(ByteVector other) const {
        return _mm_or_si128(_data, other._data);
    }

    ByteVector& operator|=(ByteVector other) {
        return (*this = (*this | other));
    }

    ByteVector operator&(ByteVector other) const {
        return _mm_and_si128(_data, other._data);
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
