// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace metrics_filtering_util {

/**
 * A trie for matching dotted paths. Each node corresponds to one segment of a path.
 * For example, path "a.b.c" has three segments, and the trie has three nested nodes.
 */
struct PathMatcherNode {
    StringMap<std::unique_ptr<PathMatcherNode>> children;
    // Set to true if the path ending at this node is a complete registered path (as opposed
    // to just being a prefix of one or more registered paths).
    bool isExactMatch = false;
};

/*
 * Builds a trie-based path matcher from an allowlist of dotted paths.
 *
 * If a path is invalid (contains empty segments, wildcards, or other unsupported syntax),
 * BadValue is thrown. Duplicate paths in the allowlist are de-duplicated automatically.
 */
PathMatcherNode buildPathMatcher(const std::vector<std::string>& paths);

/*
 * Extracts fields from a source object using a path matcher, appending them into
 * `builder` while preserving the nested structure of the source object.
 *
 * The `matcher` should be built from allowlist paths using buildPathMatcher().
 *
 * Example (assuming matcher built from paths ["a.x", "b.i.ii.x"]):
 *   source = {a: {x: 100, y: 200}, b: {i: {ii: {x: 10, y: 5}}}}
 *   result = {a: {x: 100}, b: {i: {ii: {x: 10}}}}
 *
 * Matching rules:
 *   - If a path does not exist in the source object, it produces no matches.
 *   - If a subtree contains no matched fields, it is not included in the result. For example,
 *     if the matcher includes "a.b.x" but the source object is {a: {b: {y: 5}}}, neither "a"
 *     nor "b" appear in the output.
 *   - If a field name contains a literal dot, it cannot be matched by a dotted path. For
 *     example, in {a: {"x.y": 123}}, the field "x.y" cannot be matched by path "a.x.y"
 *     because the path syntax treats dots as separators.
 *
 * Appending rules:
 *   - If a field matches the matcher, it is appended to 'builder'.
 *   - If a field already exists in 'builder', both copies are appended (BSON permits duplicate
 *     field names).
 */
void appendPaths(BSONObjBuilder& builder, const BSONObj& obj, const PathMatcherNode& matcher);

}  // namespace metrics_filtering_util
}  // namespace mongo
