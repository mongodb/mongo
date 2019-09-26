/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <vector>

#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/sort_pattern.h"

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
