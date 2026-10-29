// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace clustered_util {

/**
 * Constructs ClusteredCollectionInfo assuming legacy format {clusteredIndex: <bool>}. The
 * collection defaults to being clustered by '_id'
 */
[[MONGO_MOD_PUBLIC]] ClusteredCollectionInfo makeCanonicalClusteredInfoForLegacyFormat();

/**
 * Generates the default _id clustered index.
 */
[[MONGO_MOD_PUBLIC]] ClusteredCollectionInfo makeDefaultClusteredIdIndex();

/**
 * Constructs ClusteredCollectionInfo according to the 'indexSpec'. Constructs a 'name' by default
 * if the field is not yet defined. Stores the information is provided in the non-legacy format.
 */
[[MONGO_MOD_PRIVATE]] ClusteredCollectionInfo makeCanonicalClusteredInfo(
    ClusteredIndexSpec indexSpec);

[[MONGO_MOD_PRIVATE]] boost::optional<ClusteredCollectionInfo> parseClusteredInfo(
    const BSONElement& elem);

/**
 * Returns true if legacy format is required for the namespace.
 */
[[MONGO_MOD_PUBLIC]] bool requiresLegacyFormat(const NamespaceString& nss,
                                               const CollectionOptions& collOptions);

/**
 * listIndexes requires the ClusteredIndexSpec be formatted with an additional field 'clustered:
 * true' to indicate it is a clustered index and with the collection's default collation. If the
 * collection has the 'simple' collation this expects an empty BSONObj.
 */
[[MONGO_MOD_PUBLIC]] BSONObj formatClusterKeyForListIndexes(
    const ClusteredCollectionInfo& collInfo,
    const BSONObj& collation,
    const boost::optional<int64_t>& expireAfterSeconds,
    bool extendSimpleCollation = false);

/**
 * Returns true if the BSON object matches the collection's cluster key. Caller's should ensure
 * keyPatternObj is the 'key' of the index spec of interest, not the entire index spec BSON.
 */
[[MONGO_MOD_PUBLIC]] bool matchesClusterKey(
    const BSONObj& keyPatternObj, const boost::optional<ClusteredCollectionInfo>& collInfo);

/**
 * Returns true if the collection is clustered on the _id field.
 */
[[MONGO_MOD_PUBLIC]] bool isClusteredOnId(const boost::optional<ClusteredCollectionInfo>& collInfo);

/**
 * Returns the field name of a cluster key.
 */
[[MONGO_MOD_PUBLIC]] std::string_view getClusterKeyFieldName(const ClusteredIndexSpec& indexSpec);

/**
 * Returns the sort pattern and directions for use by the planner
 */
[[MONGO_MOD_PUBLIC]] BSONObj getSortPattern(const ClusteredIndexSpec& collInfo);

/**
 * Throws if the collection creation options are not compatible with a clustered collection.
 */
[[MONGO_MOD_PARENT_PRIVATE]] void checkCreationOptions(const CreateCommand&);

}  // namespace clustered_util
}  // namespace mongo
