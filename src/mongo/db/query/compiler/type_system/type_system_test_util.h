// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/stdx/unordered_set.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo::pipeline::type_system {

struct BSONObjBinaryLess {
    bool operator()(const BSONObj& lhs, const BSONObj& rhs) const {
        std::string_view lhsView{lhs.objdata(), static_cast<size_t>(lhs.objsize())};
        std::string_view rhsView{rhs.objdata(), static_cast<size_t>(rhs.objsize())};
        return lhsView < rhsView;
    }
};

/// Set of BSONObj compared by objdata() instead of the default (which sees 1 and 1.0 as equal).
using BSONObjBinarySet = std::set<BSONObj, BSONObjBinaryLess>;

/// The scalars a document corpus places along the path.
enum class ScalarCorpus {
    /// One value of each BSONType, except array and object.
    kAll,
    /// Only missing, null, int 1 and double 1.0, enough to tell apart how path traversal matches.
    kMatchTraversal,
};

/**
 * Returns an array containing one value of each BSONType, except array and object.
 */
std::vector<Value> scalarRepresentatives();

/**
 * Returns a corpus of documents where the values along the path take every possible BSON type,
 * including missing, null, array and array of arrays as well as objects.
 * Arrays also appear empty or with the value at position 0 or 1:
 *   [], [<nested>], [null, <nested>].
 * Objects also appear with an unrelated field with derivative name:
 *   {x: 1, x2: 1}.
 * Note that a corpus from a deeper path contains all documents from a shallower path, so the corpus
 * for 'x.y.z' is a superset of 'x.y' and 'x'.
 * 'scalars' selects which scalars appear in place of every BSON type.
 * Examples:
 * - generateDocumentCorpus("x") produces the cases where 'x' takes a value of every BSON type,
 *   including missing:
 *     {}, {x: 1}, {x: "str"}, {x: {}}, {x: []}, {x: [1]}, {x: [[1]]}, {x: [null, 1]}, ...
 * - generateDocumentCorpus("x.y") additionally produces cases where 'x' contains a path 'y', which
 *   can be in embedded object or array, and the object containing 'y' additionally takes every
 *   possible BSON type as value:
 *     {x: {y: 1}}, {x: {y: "str"}}, {x: [{y: 1}]}, {x: [[{y: 1}]]}, {x: [[{y: 'str'}]]} ...
 */
BSONObjBinarySet generateDocumentCorpus(std::string_view path,
                                        ScalarCorpus scalars = ScalarCorpus::kAll);

/**
 * Generates a corpus for each path and returns the combined result with duplicates removed.
 */
BSONObjBinarySet generateCombinedCorpus(const std::vector<std::string>& paths,
                                        ScalarCorpus scalars = ScalarCorpus::kAll);

}  // namespace mongo::pipeline::type_system
