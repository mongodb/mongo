// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns true if the point is within the range [inclusiveLower, exclusiveUpper).
 */
bool rangeContains(const BSONObj& inclusiveLower,
                   const BSONObj& exclusiveUpper,
                   const BSONObj& point);

/**
 * Returns true if the bounds specified by [inclusiveLower1, exclusiveUpper1)
 * intersects with the bounds [inclusiveLower2, exclusiveUpper2).
 */
bool rangeOverlaps(const BSONObj& inclusiveLower1,
                   const BSONObj& exclusiveUpper1,
                   const BSONObj& inclusiveLower2,
                   const BSONObj& exclusiveUpper2);

/**
 * A RangeMap is a mapping of an inclusive lower BSON key to an exclusive upper key, using standard
 * BSON woCompare.
 *
 * NOTE: For overlap testing to work correctly, there may be no overlaps present in the map itself.
 */
typedef BSONObjIndexedMap<BSONObj> RangeMap;

/**
 * Returns true if the provided range map has ranges which overlap the provided range
 * [inclusiveLower, exclusiveUpper).
 */
bool rangeMapOverlaps(const RangeMap& ranges,
                      const BSONObj& inclusiveLower,
                      const BSONObj& exclusiveUpper);

}  // namespace mongo
