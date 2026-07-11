// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * This class is used to compare "sort keys," which are the values used to determine the order of
 * documents returned by a query that requests a sort. When executing a query with a blocking sort,
 * a SortKeyGenerator stage creates a sort key for each document based on the requested sort
 * pattern, and a sort stage orders the documents using the sort keys and this comparator.
 */
class SortKeyComparator {
public:
    SortKeyComparator(const SortPattern& sortPattern);
    SortKeyComparator(const BSONObj& sortPattern);
    int operator()(const Value& lhsKey, const Value& rhsKey) const;

private:
    // The comparator does not need the entire sort pattern, just the sort direction for each
    // component.
    enum class SortDirection { kDescending, kAscending };
    std::vector<SortDirection> _pattern;
};

}  // namespace mongo
