/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
