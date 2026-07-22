// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/metrics_filtering_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

namespace mongo {
namespace metrics_filtering_util {

PathMatcherNode buildPathMatcher(const std::vector<std::string>& paths) {
    PathMatcherNode root;

    for (const auto& path : paths) {
        if (path.empty()) {
            uasserted(ErrorCodes::BadValue, "Invalid path: empty string");
        }

        if (path.find('*') != std::string::npos) {
            uasserted(ErrorCodes::BadValue,
                      fmt::format("Invalid path '{}': wildcards are not supported", path));
        }

        PathMatcherNode* node = &root;
        std::string_view segment;
        std::string_view remainder;
        auto suffix = std::string_view(path);

        while (!suffix.empty()) {
            bool hasNextSegment = str::splitOn(suffix, '.', segment, remainder);
            if (segment.empty()) {
                uasserted(ErrorCodes::BadValue,
                          fmt::format("Invalid path '{}': empty path segment", path));
            }
            if (hasNextSegment && remainder.empty()) {
                uasserted(ErrorCodes::BadValue,
                          fmt::format("Invalid path '{}': empty path segment", path));
            }

            auto& child_ptr = node->children[std::string(segment)];
            if (!child_ptr) {
                child_ptr = std::make_unique<PathMatcherNode>();
            }
            node = child_ptr.get();
            suffix = remainder;
        }
        node->isExactMatch = true;
    }

    return root;
}

void appendPaths(BSONObjBuilder& builder, const BSONObj& obj, const PathMatcherNode& node) {
    if (node.children.empty()) {
        return;
    }

    BSONObjIterator iter(obj);
    while (iter.more()) {
        BSONElement elem = iter.next();

        auto it = node.children.find(elem.fieldNameStringData());
        if (it == node.children.end()) {
            // No allowlisted path passes through this field, skip it.
            continue;
        }

        const PathMatcherNode& child = *it->second;
        if (child.isExactMatch) {
            // This field matches an allowlist path, include it as-is.
            builder.append(elem);
        } else if (!child.children.empty() && elem.type() == BSONType::object) {
            // This field has an allowlist path that traverses deeper through it, recurse into it.
            BSONObjBuilder nested;
            appendPaths(nested, elem.Obj(), child);
            BSONObj nestedObj = nested.obj();
            // Only append if the recursive extraction found matching fields.
            if (!nestedObj.isEmpty()) {
                builder.append(elem.fieldNameStringData(), nestedObj);
            }
        }
        // Otherwise, skip this field.
    }
}

}  // namespace metrics_filtering_util
}  // namespace mongo
