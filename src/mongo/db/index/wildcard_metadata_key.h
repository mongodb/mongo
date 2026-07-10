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
