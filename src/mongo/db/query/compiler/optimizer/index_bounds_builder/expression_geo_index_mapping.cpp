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


#include "mongo/db/query/compiler/optimizer/index_bounds_builder/expression_geo_index_mapping.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/geo/r2_region_coverer.h"
#include "mongo/db/hasher.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/expression_geo_index_knobs_gen.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <s2cellid.h>
#include <s2region.h>
#include <s2regioncoverer.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using std::set;

BSONObj ExpressionMapping::hash(const BSONElement& value) {
    BSONObjBuilder bob;
    bob.append("", BSONElementHasher::hash64(value, BSONElementHasher::DEFAULT_HASH_SEED));
    return bob.obj();
}

// For debugging only
static std::string toCoveringString(const GeoHashConverter& hashConverter,
                                    const set<GeoHash>& covering) {
    string result = "[";
    for (set<GeoHash>::const_iterator it = covering.begin(); it != covering.end(); ++it) {
        if (it != covering.begin())
            result += ", ";

        const GeoHash& geoHash = *it;

        result += hashConverter.unhashToBoxCovering(geoHash).toString();
        result += " (" + geoHash.toStringHex1() + ")";
    }

    return result + "]";
}

std::vector<GeoHash> ExpressionMapping::get2dCovering(const R2Region& region,
                                                      const BSONObj& indexInfoObj,
                                                      int maxCoveringCells) {
    auto result = GeoHashConverter::createFromDoc(indexInfoObj);
    MONGO_verify(result.isOK());  // We validated the parameters when creating the index.

    const auto bits = result.getValue()->getBits();
    R2RegionCoverer coverer(std::move(result.getValue()));
    coverer.setMaxLevel(bits);
    coverer.setMaxCells(maxCoveringCells);

    // TODO: Maybe slightly optimize by returning results in order
    std::vector<GeoHash> unorderedCovering;
    coverer.getCovering(region, &unorderedCovering);
    return unorderedCovering;
}

void ExpressionMapping::GeoHashsToIntervalsWithParents(
    const std::vector<GeoHash>& unorderedCovering, OrderedIntervalList* oilOut) {
    set<GeoHash> covering(unorderedCovering.begin(), unorderedCovering.end());
    for (set<GeoHash>::const_iterator it = covering.begin(); it != covering.end(); ++it) {
        const GeoHash& geoHash = *it;
        BSONObjBuilder builder;
        geoHash.appendHashMin(&builder, "");
        geoHash.appendHashMax(&builder, "");

        oilOut->intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
            builder.obj(), BoundInclusion::kIncludeBothStartAndEndKeys));
    }
}

void ExpressionMapping::cover2d(const R2Region& region,
                                const BSONObj& indexInfoObj,
                                int maxCoveringCells,
                                OrderedIntervalList* oilOut) {
    std::vector<GeoHash> unorderedCovering = get2dCovering(region, indexInfoObj, maxCoveringCells);
    GeoHashsToIntervalsWithParents(unorderedCovering, oilOut);
}

std::vector<S2CellId> ExpressionMapping::get2dsphereCovering(const S2Region& region) {
    auto minLevel = gInternalQueryS2GeoCoarsestLevel.load();
    auto maxLevel = gInternalQueryS2GeoFinestLevel.load();

    uassert(28739, "Geo coarsest level must be in range [0,30]", 0 <= minLevel && minLevel <= 30);
    uassert(28740, "Geo finest level must be in range [0,30]", 0 <= maxLevel && maxLevel <= 30);
    uassert(28741, "Geo coarsest level must be less than or equal to finest", minLevel <= maxLevel);

    S2RegionCoverer coverer;
    coverer.set_min_level(minLevel);
    coverer.set_max_level(maxLevel);
    coverer.set_max_cells(gInternalQueryS2GeoMaxCells.load());

    std::vector<S2CellId> cover;
    coverer.GetCovering(region, &cover);
    return cover;
}

void ExpressionMapping::cover2dsphere(const S2Region& region,
                                      const S2IndexingParams& indexingParams,
                                      OrderedIntervalList* oilOut) {
    std::vector<S2CellId> cover = get2dsphereCovering(region);
    S2CellIdsToIntervalsWithParents(cover, indexingParams, oilOut);
}

namespace {
bool compareIntervals(const Interval& a, const Interval& b) {
    return a.precedes(b);
}

void S2CellIdsToIntervalsUnsorted(const std::vector<S2CellId>& intervalSet,
                                  const S2IndexVersion indexVersion,
                                  OrderedIntervalList* oilOut) {
    for (const S2CellId& interval : intervalSet) {
        BSONObjBuilder b;
        if (indexVersion >= S2_INDEX_VERSION_3) {
            long long start = static_cast<long long>(interval.range_min().id());
            long long end = static_cast<long long>(interval.range_max().id());
            b.append("start", start);
            b.append("end", end);
            tassert(9911950, "", start <= end);
            oilOut->intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
                b.obj(), BoundInclusion::kIncludeBothStartAndEndKeys));
        } else {
            // for backwards compatibility, use strings
            std::string start = interval.toString();
            std::string end = start;
            end[start.size() - 1]++;
            b.append("start", start);
            b.append("end", end);
            oilOut->intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
                b.obj(), BoundInclusion::kIncludeStartKeyOnly));
        }
    }
}
}  // namespace

void ExpressionMapping::S2CellIdsToIntervals(const std::vector<S2CellId>& intervalSet,
                                             const S2IndexVersion indexVersion,
                                             OrderedIntervalList* oilOut) {
    // Order is not preserved in changing from numeric to string
    // form of index key. Therefore, sorting is deferred to after
    // intervals are made
    S2CellIdsToIntervalsUnsorted(intervalSet, indexVersion, oilOut);
    std::sort(oilOut->intervals.begin(), oilOut->intervals.end(), compareIntervals);
    // Make sure that our intervals don't overlap each other and are ordered correctly.
    // This perhaps should only be done in debug mode.
    if (!oilOut->isValidFor(1)) {
        LOGV2(6029801,
              "invalid OrderedIntervalList",
              "orderedIntervalList"_attr = redact(oilOut->toString(false)));
        MONGO_UNREACHABLE_TASSERT(9911963);
    }
}

void ExpressionMapping::S2CellIdsToIntervalsWithParents(const std::vector<S2CellId>& intervalSet,
                                                        const S2IndexingParams& indexParams,
                                                        OrderedIntervalList* oilOut) {
    // There may be duplicates when going up parent cells if two cells share a parent
    stdx::unordered_set<S2CellId> exactSet;
    for (const S2CellId& interval : intervalSet) {
        S2CellId coveredCell = interval;
        // Look at the cells that cover us.  We want to look at every cell that contains the
        // covering we would index on if we were to insert the query geometry.  We generate
        // the would-index-with-this-covering and find all the cells strictly containing the
        // cells in that set, until we hit the coarsest indexed cell.  We use equality, not
        // a prefix match.  Why not prefix?  Because we've already looked at everything
        // finer or as fine as our initial covering.
        //
        // Say we have a fine point with cell id 212121, we go up one, get 21212, we don't
        // want to look at cells 21212[not-1] because we know they're not going to intersect
        // with 212121, but entries inserted with cell value 21212 (no trailing digits) may.
        // And we've already looked at points with the cell id 211111 from the regex search
        // created above, so we only want things where the value of the last digit is not
        // stored (and therefore could be 1).

        while (coveredCell.level() > indexParams.coarsestIndexedLevel) {
            // Add the parent cell of the currently covered cell since we aren't at the
            // coarsest level yet
            // NOTE: Be careful not to generate cells strictly less than the
            // coarsestIndexedLevel - this can result in S2 failures when level < 0.

            coveredCell = coveredCell.parent();
            exactSet.insert(coveredCell);
        }
    }

    for (const S2CellId& exact : exactSet) {
        BSONObj exactBSON = index2dsphere::S2CellIdToIndexKey(exact, indexParams.indexVersion);
        oilOut->intervals.push_back(IndexBoundsBuilder::makePointInterval(exactBSON));
    }

    S2CellIdsToIntervalsUnsorted(intervalSet, indexParams.indexVersion, oilOut);
    std::sort(oilOut->intervals.begin(), oilOut->intervals.end(), compareIntervals);
    // Make sure that our intervals don't overlap each other and are ordered correctly.
    // This perhaps should only be done in debug mode.
    if (!oilOut->isValidFor(1)) {
        LOGV2(6029802,
              "invalid OrderedIntervalList",
              "orderedIntervalList"_attr = redact(oilOut->toString(false)));
        MONGO_UNREACHABLE_TASSERT(9911964);
    }
}

}  // namespace mongo
