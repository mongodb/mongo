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

#include "mongo/db/index/s2_common.h"

#include <s2.h>
#include <s2cellid.h>
#include <s2regioncoverer.h>

#include <util/math/vector3.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/query/collation/collator_interface.h"

#include <memory>
#include <ostream>

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

namespace index2dsphere {

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
    if (indexVersion >= S2_INDEX_VERSION_3) {
        // The size of an index BSONObj in S2 index version 3 is 15 bytes.
        // total size (4 bytes)  |  type code 0x12 (1)  |  field name "" 0x00 (1)  |
        // long long cell id (8) | EOO (1)
        BSONObjBuilder b(15);
        b.append("", static_cast<long long>(cellId.id()));
        return b.obj();
    }

    // The size of an index BSONObj in older versions is 10 ~ 40 bytes.
    // total size (4 bytes)  |  type code 0x12 (1)  |  field name "" 0x00 (1)  |
    // cell id string (2 ~ 32) 0x00 (1) | EOO (1)
    BSONObjBuilder b;
    b.append("", cellId.ToString());
    // Return a copy so its buffer size fits the object size.
    return b.obj().copy();
}

void S2CellIdToIndexKeyStringAppend(const S2CellId& cellId,
                                    S2IndexVersion indexVersion,
                                    const std::vector<key_string::HeapBuilder>& keysToAdd,
                                    std::vector<key_string::HeapBuilder>* out,
                                    key_string::Version keyStringVersion,
                                    Ordering ordering) {
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
    if (indexVersion >= S2_INDEX_VERSION_3) {
        if (keysToAdd.empty()) {
            out->emplace_back(keyStringVersion, ordering);
            out->back().appendNumberLong(cellId.id());
        }
        for (const auto& ks : keysToAdd) {
            out->emplace_back(ks);
            out->back().appendNumberLong(cellId.id());
        }
        return;
    }

    if (keysToAdd.empty()) {
        out->emplace_back(keyStringVersion, ordering);
        out->back().appendString(cellId.ToString());
    }
    for (const auto& ks : keysToAdd) {
        out->emplace_back(ks);
        out->back().appendString(cellId.ToString());
    }
}

void initialize2dsphereParams(const BSONObj& infoObj,
                              const CollatorInterface* collator,
                              S2IndexingParams* out) {
    // Set up basic params.
    out->collator = collator;
    out->maxKeysPerInsert = 200;

    // Near distances are specified in meters...sometimes.
    out->radius = kRadiusOfEarthInMeters;

    static const std::string kIndexVersionFieldName("2dsphereIndexVersion");
    static const std::string kFinestIndexedLevel("finestIndexedLevel");
    static const std::string kCoarsestIndexedLevel("coarsestIndexedLevel");

    long long indexVersion;

    // Determine which version of this index we're using.  If none was set in the descriptor,
    // assume S2_INDEX_VERSION_1 (alas, the first version predates the existence of the version
    // field).
    Status status = bsonExtractIntegerFieldWithDefault(
        infoObj, kIndexVersionFieldName, S2_INDEX_VERSION_1, &indexVersion);
    uassertStatusOK(status);

    out->indexVersion = static_cast<S2IndexVersion>(indexVersion);

    // Note: In version > 2, these levels are for non-points.
    // Points are always indexed to the finest level.
    // Default levels were optimized for buildings and state regions
    long long defaultFinestIndexedLevel = S2::kAvgEdge.GetClosestLevel(110.0 / out->radius);
    long long defaultCoarsestIndexedLevel =
        S2::kAvgEdge.GetClosestLevel(2000 * 1000.0 / out->radius);
    long long defaultMaxCellsInCovering = 20;

    if (out->indexVersion <= S2_INDEX_VERSION_2) {
        defaultFinestIndexedLevel = S2::kAvgEdge.GetClosestLevel(500.0 / out->radius);
        defaultCoarsestIndexedLevel = S2::kAvgEdge.GetClosestLevel(100.0 * 1000 / out->radius);
        defaultMaxCellsInCovering = 50;
    }

    long long finestIndexedLevel, coarsestIndexedLevel, maxCellsInCovering;

    status = bsonExtractIntegerFieldWithDefault(
        infoObj, "finestIndexedLevel", defaultFinestIndexedLevel, &finestIndexedLevel);
    uassertStatusOK(status);

    status = bsonExtractIntegerFieldWithDefault(
        infoObj, "coarsestIndexedLevel", defaultCoarsestIndexedLevel, &coarsestIndexedLevel);
    uassertStatusOK(status);

    status = bsonExtractIntegerFieldWithDefault(
        infoObj, "maxCellsInCovering", defaultMaxCellsInCovering, &maxCellsInCovering);
    uassertStatusOK(status);

    // This is advisory.
    out->maxCellsInCovering = maxCellsInCovering;

    // These are not advisory.
    out->finestIndexedLevel = finestIndexedLevel;
    out->coarsestIndexedLevel = coarsestIndexedLevel;

    uassert(16747, "coarsestIndexedLevel must be >= 0", out->coarsestIndexedLevel >= 0);
    uassert(16748, "finestIndexedLevel must be <= 30", out->finestIndexedLevel <= 30);
    uassert(16749,
            "finestIndexedLevel must be >= coarsestIndexedLevel",
            out->finestIndexedLevel >= out->coarsestIndexedLevel);


    massert(17395,
            str::stream() << "unsupported geo index version { " << kIndexVersionFieldName << " : "
                          << out->indexVersion << " }, only support versions: ["
                          << S2_INDEX_VERSION_1 << "," << S2_INDEX_VERSION_2 << ","
                          << S2_INDEX_VERSION_3 << "]",
            out->indexVersion == S2_INDEX_VERSION_3 || out->indexVersion == S2_INDEX_VERSION_2 ||
                out->indexVersion == S2_INDEX_VERSION_1);
}
}  // namespace index2dsphere
}  // namespace mongo
