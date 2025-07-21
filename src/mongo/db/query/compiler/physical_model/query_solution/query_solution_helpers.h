/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

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
