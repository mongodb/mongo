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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/simple_bsonobj_comparator.h"

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
