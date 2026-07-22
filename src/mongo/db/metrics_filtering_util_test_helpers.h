// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/metrics_filtering_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace metrics_filtering_util {

/**
 * Returns whether a path exists in the matcher and is marked as an exact match.
 * Throws an assertion if the path does not exist in the matcher.
 */
bool pathIsExactMatch(const PathMatcherNode& matcher, std::string_view path);

/**
 * Recursively counts all nodes in the matcher trie that are marked as exact matches.
 */
size_t countExactMatches(const PathMatcherNode& node);

}  // namespace metrics_filtering_util
}  // namespace mongo
