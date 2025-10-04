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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/util/modules.h"

#include <vector>

#include <s2cellid.h>
#include <s2region.h>

class S2CellId;
class S2Region;

namespace mongo {

/**
 * Functions that compute expression index mappings for geospatial queries.
 *
 * TODO: I think we could structure this more generally with respect to planning.
 */
class ExpressionMapping {
public:
    static BSONObj hash(const BSONElement& value);

    static std::vector<GeoHash> get2dCovering(const R2Region& region,
                                              const BSONObj& indexInfoObj,
                                              int maxCoveringCells);

    static void GeoHashsToIntervalsWithParents(const std::vector<GeoHash>& unorderedCovering,
                                               OrderedIntervalList* oilOut);

    static void cover2d(const R2Region& region,
                        const BSONObj& indexInfoObj,
                        int maxCoveringCells,
                        OrderedIntervalList* oilOut);

    static std::vector<S2CellId> get2dsphereCovering(const S2Region& region);

    static void S2CellIdsToIntervals(const std::vector<S2CellId>& intervalSet,
                                     S2IndexVersion indexVersion,
                                     OrderedIntervalList* oilOut);

    // Creates an ordered interval list from range intervals and
    // traverses cell parents for exact intervals up to coarsestIndexedLevel
    static void S2CellIdsToIntervalsWithParents(const std::vector<S2CellId>& interval,
                                                const S2IndexingParams& indexParams,
                                                OrderedIntervalList* out);

    static void cover2dsphere(const S2Region& region,
                              const S2IndexingParams& indexParams,
                              OrderedIntervalList* oilOut);
};

}  // namespace mongo
