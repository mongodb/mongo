// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace wildcard_planning {
/**
 * Returns true if the given IndexScanNode is a $** scan whose bounds overlap the object type
 * bracket. Scans whose bounds include the object bracket have certain limitations for planning
 * purposes; for instance, they cannot provide covered results or be converted to DISTINCT_SCAN.
 */
inline bool isWildcardObjectSubpathScan(const IndexEntry& index, const IndexBounds& bounds) {
    // If the node is not a $** index scan, return false immediately.
    if (index.type != IndexType::INDEX_WILDCARD) {
        return false;
    }

    // Check the bounds on the query field for any intersections with the object type bracket.
    return bounds.fields[index.wildcardFieldPos].boundsOverlapObjectTypeBracket();
}

inline bool isWildcardObjectSubpathScan(const IndexScanNode* node) {
    return node && isWildcardObjectSubpathScan(node->index, node->bounds);
}
}  // namespace wildcard_planning
}  // namespace mongo
