// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/index/geo/s2_common.h"
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
