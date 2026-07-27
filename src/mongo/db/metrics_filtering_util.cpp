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

            // Check for array path syntax.
            if (segment == "$[]") {
                if (node == &root) {
                    uasserted(ErrorCodes::BadValue,
                              fmt::format("Invalid path '{}': array segment '.$[]' must follow a "
                                          "field name (e.g., 'a.$[].fieldName')",
                                          path));
                }
                if (!hasNextSegment) {
                    uasserted(
                        ErrorCodes::BadValue,
                        fmt::format("Invalid path '{}': array segment '.$[]' must have a field "
                                    "to extract (e.g., 'a.$[].fieldName')",
                                    path));
                }
                // Check if the next segment is also $[], which would be consecutive arrays.
                std::string_view nextSegment;
                std::string_view nextRemainder;
                if (str::splitOn(remainder, '.', nextSegment, nextRemainder) &&
                    nextSegment == "$[]") {
                    uasserted(
                        ErrorCodes::BadValue,
                        fmt::format("Invalid path '{}': consecutive array segments '.$[].$[]' "
                                    "are not supported",
                                    path));
                }
                // Mark the current node as having array traversal.
                node->isArrayPath = true;
                suffix = remainder;
                continue;
            }

            // Check for invalid bracket syntax.
            if (segment.find('[') != std::string::npos || segment.find(']') != std::string::npos) {
                uasserted(ErrorCodes::BadValue,
                          fmt::format("Invalid path '{}': unsupported bracket syntax '{}'. "
                                      "Only '.$[]' is supported for array traversal.",
                                      path,
                                      segment));
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
        } else if (child.isArrayPath) {
            // This field should be traversed as an array.
            if (elem.type() == BSONType::array) {
                BSONArrayBuilder arrayBuilder;
                for (BSONElement arrayElem : elem.Obj()) {
                    if (arrayElem.type() == BSONType::object) {
                        BSONObjBuilder elementBuilder;
                        appendPaths(elementBuilder, arrayElem.Obj(), child);
                        BSONObj extractedElement = elementBuilder.obj();
                        // Only append non-empty extracted elements.
                        if (!extractedElement.isEmpty()) {
                            arrayBuilder.append(extractedElement);
                        }
                    }
                    // Skip non-object array elements.
                }
                BSONArray resultArray = arrayBuilder.arr();
                // Only append the array field if it contains elements.
                if (!resultArray.isEmpty()) {
                    builder.append(elem.fieldNameStringData(), resultArray);
                }
            }
            // If field is not an array, skip it (don't try to traverse as object).
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
