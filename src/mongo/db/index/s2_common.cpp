/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/index/s2_common.h"

#include <cstdlib>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "third_party/s2/s2cellid.h"
#include "third_party/s2/s2regioncoverer.h"

namespace mongo {

// Points will only be indexed at this level
const int kPointIndexedLevel = S2::kMaxCellLevel;

std::string S2IndexingParams::toString() const {
    std::stringstream ss;
    ss << "maxKeysPerInsert: " << maxKeysPerInsert << std::endl;
    ss << "maxCellsInCovering: " << maxCellsInCovering << std::endl;
    ss << "finestIndexedLevel: " << finestIndexedLevel << std::endl;
    ss << "coarsestIndexedLevel: " << coarsestIndexedLevel << std::endl;
    ss << "indexVersion: " << indexVersion << std::endl;
    if (collator) {
        ss << "collation: " << collator->getSpec().toBSON() << std::endl;
    }
    return ss.str();
}

void S2IndexingParams::configureCoverer(const GeometryContainer& geoContainer,
                                        S2RegionCoverer* coverer) const {
    // Points indexed to the finest level was introduced in version 3
    // For backwards compatibility, only do this if the version is > 2
    if (indexVersion >= S2_INDEX_VERSION_3 && geoContainer.isPoint()) {
        coverer->set_min_level(kPointIndexedLevel);
        coverer->set_max_level(kPointIndexedLevel);
    } else {
        coverer->set_min_level(coarsestIndexedLevel);
        coverer->set_max_level(finestIndexedLevel);
    }

    // This is advisory; the two above are strict.
    coverer->set_max_cells(maxCellsInCovering);
}

BSONObj S2CellIdToIndexKey(const S2CellId& cellId, S2IndexVersion indexVersion) {
    // The range of an unsigned long long is
    // |-----------------|------------------|
    // 0                2^32               2^64 - 1
    // 000...           100...             111...
    // The range of a signed long long is
    // |-----------------|------------------|
    // -2^63             0                 2^63 - 1
    // 100...           000...             011...
    // S2 gives us an unsigned long long, and we need
    // to use signed long longs for the index.
    //
    // The relative ordering may be changed with unsigned
    // numbers around 2^32 being converted to signed
    //
    // However, because a single cell cannot span over
    // more than once face, individual intervals will
    // never cross that threshold. Thus, scans will still
    // produce the same results.
    BSONObjBuilder b;
    if (indexVersion >= S2_INDEX_VERSION_3) {
        b.append("", static_cast<long long>(cellId.id()));
    } else {
        b.append("", cellId.ToString());
    }
    return b.obj();
}
}  // namespace mongo
