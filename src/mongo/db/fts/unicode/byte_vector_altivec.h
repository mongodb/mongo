/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <altivec.h>
#undef vector
#undef bool

#include <cstdint>

#include "mongo/config.h"
#include "mongo/platform/bits.h"

namespace mongo {
namespace unicode {

/**
 * A sequence of bytes that can be manipulated using vectorized instructions.
 *
 * This is specific to the use case in mongo::unicode::String and not intended as a general purpose
 * vector class.
 *
 * This specialization offers acceleration for ppc64le
 */
class ByteVector {
public:
    using Native = __vector signed char;

    // Logically 16 bits, but this is the actual output type of vec_vbpermq
    using Mask = uint64_t;

    using Scalar = int8_t;
    static const int size = sizeof(Native);

    /**
     * Sets all bytes to 0.
     */
    ByteVector() {
        _data = vec_splat_s8(0);
    }

    /**
     * Sets all bytes to val.
     */
    explicit ByteVector(Scalar val) {
        _data = vec_splats(val);
    }

    /**
     * Load a vector from a potentially unaligned location.
     */
    static ByteVector load(const void* ptr) {
        // This function is documented as taking an unaligned pointer.
        return vec_vsx_ld(0, reinterpret_cast<const Native*>(ptr));
    }

    /**
     * Store this vector to a potentially unaligned location.
     */
    void store(void* ptr) const {
        // This function is documented as taking an unaligned pointer.
        vec_vsx_st(_data, 0, reinterpret_cast<Native*>(ptr));
    }

    /**
     * Returns a bitmask with the high bit from each byte.
     */
    Mask maskHigh() const {
        // The bits array represents the api indexes of the left to right high bits.
        //
        // This seems really wrong, but the indexes you hand to vec_vbpermq are actually mirrored on
        // little endian (the interface seems more designed for big endian).  You'd do 7, 15, etc on
        // big endian by comparison.
        const Native bits = {120, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0};

        return vec_extract(vec_vbpermq(_data, bits), MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX);
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
        return (Native)vec_cmpeq(_data, ByteVector(val)._data);
    }
    ByteVector compareLT(Scalar val) const {
        return (Native)vec_cmplt(_data, ByteVector(val)._data);
    }
    ByteVector compareGT(Scalar val) const {
        return (Native)vec_cmpgt(_data, ByteVector(val)._data);
    }

    ByteVector operator|(ByteVector other) const {
        return (Native)vec_or(_data, other._data);
    }

    ByteVector& operator|=(ByteVector other) {
        return (*this = (*this | other));
    }

    ByteVector operator&(ByteVector other) const {
        return (Native)vec_and(_data, other._data);
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
