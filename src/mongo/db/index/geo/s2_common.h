// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/version_context.h"
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
    S2_INDEX_VERSION_3 = 3,

    // The fourth version of the S2 index. Changed parsing order for object-type
    // geometry elements to try GeoJSON parsing before legacy point parsing.
    S2_INDEX_VERSION_4 = 4
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

/**
 * Returns the default S2 index version based on the feature flag.
 * If the feature flag for version 4 is enabled, returns S2_INDEX_VERSION_4,
 * otherwise returns S2_INDEX_VERSION_3.
 *
 * @param versionContext The version context to use for feature flag checks.
 *                       Defaults to kVersionContextIgnored_UNSAFE.
 */
S2IndexVersion getDefaultS2IndexVersion(
    const VersionContext& versionContext = kVersionContextIgnored_UNSAFE);
}  // namespace index2dsphere
}  // namespace mongo
