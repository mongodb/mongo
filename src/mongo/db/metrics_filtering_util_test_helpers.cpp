// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/metrics_filtering_util_test_helpers.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace metrics_filtering_util {

bool pathIsExactMatch(const PathMatcherNode& matcher, std::string_view path) {
    const auto* node = &matcher;
    std::string_view segment, remainder;
    auto suffix = path;

    while (!suffix.empty()) {
        str::splitOn(suffix, '.', segment, remainder);
        auto it = node->children.find(std::string(segment));
        ASSERT(it != node->children.end())
            << "path '" << path << "' missing segment '" << segment << "'";
        node = it->second.get();
        suffix = remainder;
    }
    return node->isExactMatch;
}

size_t countExactMatches(const PathMatcherNode& node) {
    size_t count = node.isExactMatch ? 1 : 0;
    for (const auto& [_, child] : node.children) {
        count += countExactMatches(*child);
    }
    return count;
}

}  // namespace metrics_filtering_util
}  // namespace mongo
