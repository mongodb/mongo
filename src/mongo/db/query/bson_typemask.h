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


#include "mongo/bson/bsontypes.h"

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
inline uint32_t getBSONTypeMask(BSONType t) noexcept {
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

const uint32_t kNumberMask = getBSONTypeMask(BSONType::numberInt) |
    getBSONTypeMask(BSONType::numberLong) | getBSONTypeMask(BSONType::numberDouble) |
    getBSONTypeMask(BSONType::numberDecimal);

const uint32_t kDateMask = getBSONTypeMask(BSONType::date);

}  // namespace mongo
