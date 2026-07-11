// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"

#include <set>

namespace mongo {

/**
 * Decodes the single multikey path from one already-BSON-materialized wildcard metadata key.
 * Skips leading MinKey placeholders, expects the sentinel integer 1, then reads the path string.
 * Tasserts on any deviation from the documented metadata key format defined by
 * `WildcardKeyGenerator::makeMultikeyMetadataKey`.
 */
FieldRef decodeWildcardMultikeyMetadataPath(const BSONObj& keyBson);

/**
 * Decodes the set of multikey path FieldRefs from a `KeyStringSet` of wildcard index metadata
 * keys. Each key follows the format documented for `makeMultikeyMetadataKey`.
 */
[[MONGO_MOD_PUBLIC]] std::set<FieldRef> extractWildcardMultikeyPathsFromMetadataKeys(
    const KeyStringSet& metadataKeys, const Ordering& ordering);

}  // namespace mongo
