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

#include "mongo/db/query/expression_index.h"

#include <iostream>

#include "third_party/s2/s2regioncoverer.h"

#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/r2_region_coverer.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_indexing_params.h"
#include "mongo/db/index/s2_keys.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/query/expression_index_knobs.h"

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

void ExpressionMapping::cover2d(const R2Region& region,
                                const BSONObj& indexInfoObj,
                                int maxCoveringCells,
                                OrderedIntervalList* oil) {
    GeoHashConverter::Parameters hashParams;
    Status paramStatus = GeoHashConverter::parseParameters(indexInfoObj, &hashParams);
    verify(paramStatus.isOK());  // We validated the parameters when creating the index

    GeoHashConverter hashConverter(hashParams);
    R2RegionCoverer coverer(&hashConverter);
    coverer.setMaxLevel(hashConverter.getBits());
    coverer.setMaxCells(maxCoveringCells);

    // TODO: Maybe slightly optimize by returning results in order
    vector<GeoHash> unorderedCovering;
    coverer.getCovering(region, &unorderedCovering);
    set<GeoHash> covering(unorderedCovering.begin(), unorderedCovering.end());

    for (set<GeoHash>::const_iterator it = covering.begin(); it != covering.end(); ++it) {
        const GeoHash& geoHash = *it;
        BSONObjBuilder builder;
        geoHash.appendHashMin(&builder, "");
        geoHash.appendHashMax(&builder, "");

        oil->intervals.push_back(IndexBoundsBuilder::makeRangeInterval(builder.obj(), true, true));
    }
}

void ExpressionMapping::cover2dsphere(const S2Region& region,
                                      const S2IndexingParams& indexingParams,
                                      OrderedIntervalList* oilOut) {
    uassert(28739,
            "Geo coarsest level must be in range [0,30]",
            0 <= internalQueryS2GeoCoarsestLevel && internalQueryS2GeoCoarsestLevel <= 30);
    uassert(28740,
            "Geo finest level must be in range [0,30]",
            0 <= internalQueryS2GeoFinestLevel && internalQueryS2GeoFinestLevel <= 30);
    uassert(28741,
            "Geo coarsest level must be less than or equal to finest",
            internalQueryS2GeoCoarsestLevel <= internalQueryS2GeoFinestLevel);

    S2RegionCoverer coverer;
    coverer.set_min_level(internalQueryS2GeoCoarsestLevel);
    coverer.set_max_level(internalQueryS2GeoFinestLevel);
    coverer.set_max_cells(internalQueryS2GeoMaxCells);

    std::vector<S2CellId> cover;
    coverer.GetCovering(region, &cover);

    S2CellIdsToIntervalsWithParents(cover, indexingParams, oilOut);
}

}  // namespace mongo
