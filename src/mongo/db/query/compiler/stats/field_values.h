// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/field_path.h"

#include <span>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>

namespace mongo::stats {

using FieldValueTuple = absl::InlinedVector<sbe::value::TagValueView, 3>;

/**
 * Extracts borrowed SBE views of the values at 'paths' in 'doc'; 'doc' must outlive the returned
 * views. Arrays along a path are traversed as in a multikey index, and every distinct value found
 * is returned, so a document can produce several tuples: the cross product of the values of each
 * path, each tuple in path order. A path that resolves to no value contributes Nothing.
 * The second result is true if any requested path encounters an array, including trailing and
 * empty arrays, or yields an object value. The extracted tuples are returned even when this flag
 * is true, so callers decide whether to collect them.
 * Throws ExceededMemoryLimit if a document would produce more than 100,000 tuples.
 */
std::pair<std::vector<FieldValueTuple>, bool> extractFieldValues(const BSONObj& doc,
                                                                 std::span<const FieldPath> paths);

}  // namespace mongo::stats
