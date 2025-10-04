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
#include "mongo/bson/ordering.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <vector>

class S2CellId;
class S2RegionCoverer;

namespace mongo {

class GeometryContainer;

// An enum describing the version of an S2 index.
enum S2IndexVersion {
    // The first version of the S2 index, introduced in MongoDB 2.4.0.  Compatible with MongoDB
    // 2.4.0 and later.  Supports the following GeoJSON objects: Point, LineString, Polygon.
    S2_INDEX_VERSION_1 = 1,

    // The second version of the S2 index, introduced in MongoDB 2.6.0.  Compatible with
    // MongoDB 2.6.0 and later.  Introduced support for the following GeoJSON objects:
    // MultiPoint, MultiLineString, MultiPolygon, GeometryCollection.
    S2_INDEX_VERSION_2 = 2,

    // The third version of the S2 index, introduced in MongoDB 3.2.0. Introduced
    // performance improvements and changed the key type from string to numeric
    S2_INDEX_VERSION_3 = 3
};

struct S2IndexingParams {
    // Since we take the cartesian product when we generate keys for an insert,
    // we need a cap.
    std::size_t maxKeysPerInsert;
    // This is really an advisory parameter that we pass to the cover generator.  The
    // finest/coarsest index level determine the required # of cells.
    int maxCellsInCovering;
    // What's the finest grained level that we'll index for non-points?
    int finestIndexedLevel;
    // And, what's the coarsest for non-points?  When we search in larger coverings
    // we know we can stop here -- we index nothing coarser than this.
    int coarsestIndexedLevel;
    // Version of this index (specific to the index type).
    S2IndexVersion indexVersion;
    // Radius of the earth in meters
    double radius;
    // Null if this index orders strings according to the simple binary compare. If non-null,
    // represents the collator used to generate index keys for indexed strings.
    const CollatorInterface* collator = nullptr;

    std::string toString() const;

    void configureCoverer(const GeometryContainer& geoContainer, S2RegionCoverer* coverer) const;
};

namespace index2dsphere {
BSONObj S2CellIdToIndexKey(const S2CellId& cellId, S2IndexVersion indexVersion);

void S2CellIdToIndexKeyStringAppend(const S2CellId& cellId,
                                    S2IndexVersion indexVersion,
                                    const std::vector<key_string::HeapBuilder>& keysToAdd,
                                    std::vector<key_string::HeapBuilder>* out,
                                    key_string::Version keyStringVersion,
                                    Ordering ordering);

void initialize2dsphereParams(const BSONObj& infoObj,
                              const CollatorInterface* collator,
                              S2IndexingParams* out);
}  // namespace index2dsphere
}  // namespace mongo
