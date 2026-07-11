// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/bson/bsontypes.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * When evaluating the $type operator, it's handy to be able to represent a set of BSONTypes as a
 * bitmask, where each bit in the bitmask represents a different BSONType. Specifically, we use
 * bit 0 to represent the MinKey type, bit 31 to represent the MaxKey type, and for all other
 * BSONTypes we use bit N to represent (BSONType)(N). Because in practice aren't any real values
 * of type EOO, there is no bit assigned to represent the EOO type.
 *
 * getBSONTypeMask(t) returns a mask where the bit representing BSONType t is 1 and all the other
 * bits are 0. For EOO, getBSONTypeMask() returns 0.
 *
 * To build a bitmask representing a set containing multiple BSONTypes t1,t2,..,tN, the bitwise-or
 * operator can be used to combine results of getBSONTypeMask() like so:
 *   getBSONTypeMask(t1) | getBSONTypeMask(t2) | .. | getBSONTypeMask(tN)
 *
 * To test whether a bitmask representing a set of BSONTypes `mask` contains a given BSONType t,
 * the bitwise-and operator can be used like so:
 *   (mask & getBSONTypeMask(t)) != 0
 */
inline constexpr uint32_t getBSONTypeMask(BSONType t) noexcept {
    switch (t) {
        case BSONType::eoo:
            return 0;
        case BSONType::minKey:
            return uint32_t{1} << 0;
        case BSONType::maxKey:
            return uint32_t{1} << 31;
        default:
            return uint32_t{1} << static_cast<uint8_t>(t);
    }
}

constexpr uint32_t kNumberMask = getBSONTypeMask(BSONType::numberInt) |
    getBSONTypeMask(BSONType::numberLong) | getBSONTypeMask(BSONType::numberDouble) |
    getBSONTypeMask(BSONType::numberDecimal);

constexpr uint32_t kDateMask = getBSONTypeMask(BSONType::date);

}  // namespace mongo
