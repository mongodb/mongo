// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

[[MONGO_MOD_PUBLIC]];

/**
 * Helpers shared between the primary-side serializer (index_catalog_entry_impl.cpp) and the
 * secondary-side apply handler (repl/oplog.cpp) for the wildcard variant of the
 * `setMultikeyMetadata` oplog entry.
 *
 * Format of the oplog 'paths' field for wildcard indexes: an array of field path strings,
 * e.g. ["a", "b.c"]. The primary extracts the paths from the in-index metadata KeyStrings;
 * the secondary rebuilds KeyStrings from those paths using the target index's properties.
 */
namespace mongo::set_multikey_metadata_oplog_helpers {

/**
 * Extracts field path strings from wildcard metadata KeyStrings.
 * Metadata keys encode: { "": MinKey, ..., "": 1, "": "field.path", "": MinKey, ... }.
 */
std::vector<std::string> extractFieldPathsFromMetadataKeys(const KeyStringSet& metadataKeys,
                                                           const Ordering& ordering);

/**
 * Serializes wildcard field path strings into a BSON array suitable for the 'paths' field of
 * the `setMultikeyMetadata` oplog entry.
 */
BSONObj fieldPathsToBSON(const std::vector<std::string>& fieldPaths);

/**
 * Regenerates wildcard multikey metadata KeyStrings from the serialized field paths in a
 * `setMultikeyMetadata` oplog entry.
 *
 * Input format — 'pathsObj' is the oplog entry's 'paths' field: a BSON array of field-path
 *   strings, one per multikey path. Example: `["a", "b.c"]`.
 *
 * 'version', 'ordering', 'rsKeyFormat' come from the target index's SortedDataInterface so
 * regenerated keys are identical to the keys produced by the primary at write time.
 */
KeyStringSet regenerateMetadataKeysFromFieldPaths(const BSONObj& pathsObj,
                                                  key_string::Version version,
                                                  Ordering ordering,
                                                  KeyFormat rsKeyFormat,
                                                  const BSONObj& keyPattern);

}  // namespace mongo::set_multikey_metadata_oplog_helpers
