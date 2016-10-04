/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <vector>

#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/index_bounds_builder.h"  // For OrderedIntervalList

class S2CellId;
class S2Region;

namespace mongo {

/**
 * Functions that compute expression index mappings.
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
                                     const S2IndexVersion indexVersion,
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
