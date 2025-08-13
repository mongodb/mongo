/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace clustered_util {

/**
 * Constructs ClusteredCollectionInfo assuming legacy format {clusteredIndex: <bool>}. The
 * collection defaults to being clustered by '_id'
 */
ClusteredCollectionInfo makeCanonicalClusteredInfoForLegacyFormat();

/**
 * Generates the default _id clustered index.
 */
ClusteredCollectionInfo makeDefaultClusteredIdIndex();

/**
 * Constructs ClusteredCollectionInfo according to the 'indexSpec'. Constructs a 'name' by default
 * if the field is not yet defined. Stores the information is provided in the non-legacy format.
 */
ClusteredCollectionInfo makeCanonicalClusteredInfo(ClusteredIndexSpec indexSpec);

boost::optional<ClusteredCollectionInfo> parseClusteredInfo(const BSONElement& elem);

/**
 * Returns true if legacy format is required for the namespace.
 */
bool requiresLegacyFormat(const NamespaceString& nss, const CollectionOptions& collOptions);

/**
 * listIndexes requires the ClusteredIndexSpec be formatted with an additional field 'clustered:
 * true' to indicate it is a clustered index and with the collection's default collation. If the
 * collection has the 'simple' collation this expects an empty BSONObj.
 */
BSONObj formatClusterKeyForListIndexes(const ClusteredCollectionInfo& collInfo,
                                       const BSONObj& collation,
                                       const boost::optional<int64_t>& expireAfterSeconds);

/**
 * Returns true if the BSON object matches the collection's cluster key. Caller's should ensure
 * keyPatternObj is the 'key' of the index spec of interest, not the entire index spec BSON.
 */
bool matchesClusterKey(const BSONObj& keyPatternObj,
                       const boost::optional<ClusteredCollectionInfo>& collInfo);

/**
 * Returns true if the collection is clustered on the _id field.
 */
bool isClusteredOnId(const boost::optional<ClusteredCollectionInfo>& collInfo);

/**
 * Returns the field name of a cluster key.
 */
StringData getClusterKeyFieldName(const ClusteredIndexSpec& indexSpec);

/**
 * Returns the sort pattern and directions for use by the planner
 */
BSONObj getSortPattern(const ClusteredIndexSpec& collInfo);

/**
 * Throws if the collection creation options are not compatible with a clustered collection.
 */
void checkCreationOptions(const CreateCommand&);

}  // namespace clustered_util
}  // namespace mongo
