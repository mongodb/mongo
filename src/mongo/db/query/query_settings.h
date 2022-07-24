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

#include <boost/optional.hpp>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

/**
 * Filter indicating whether an index entry is in the set of allowed indices.
 */
class AllowedIndicesFilter {
private:
    AllowedIndicesFilter(const AllowedIndicesFilter&) = delete;
    AllowedIndicesFilter& operator=(const AllowedIndicesFilter&) = delete;

public:
    AllowedIndicesFilter(const BSONObjSet& indexKeyPatterns,
                         const stdx::unordered_set<std::string>& indexNames);
    AllowedIndicesFilter(AllowedIndicesFilter&& other) = default;

    AllowedIndicesFilter& operator=(AllowedIndicesFilter&& other) = default;

    /**
     * Returns true if entry is allowed by the filter either because it has a matching key pattern
     * or index name, and returns false otherwise.
     */
    bool allows(const IndexEntry& entry) const {
        return indexKeyPatterns.find(entry.keyPattern) != indexKeyPatterns.end() ||
            indexNames.find(entry.identifier.catalogName) != indexNames.end();
    }

    // These are the index key patterns and names that
    // we will use to override the indexes retrieved from
    // the index catalog.
    BSONObjSet indexKeyPatterns;
    stdx::unordered_set<std::string> indexNames;
};

/**
 * Value type for query settings.
 * Holds:
 *     query shape (query, sort, projection, collation)
 *     unordered_set of index specs
 */
class AllowedIndexEntry {
public:
    AllowedIndexEntry(const BSONObj& query,
                      const BSONObj& sort,
                      const BSONObj& projection,
                      const BSONObj& collation,
                      const BSONObjSet& indexKeyPatterns,
                      const stdx::unordered_set<std::string>& indexNames);

    // query, sort, projection, and collation collectively represent the query shape that we are
    // storing hint overrides for.
    BSONObj query;
    BSONObj sort;
    BSONObj projection;
    BSONObj collation;

    // These are the index key patterns and names that
    // we will use to override the indexes retrieved from
    // the index catalog.
    BSONObjSet indexKeyPatterns;
    stdx::unordered_set<std::string> indexNames;
};

/**
 * Holds the index filters in a collection.
 */
class QuerySettings {
private:
    QuerySettings(const QuerySettings&) = delete;
    QuerySettings& operator=(const QuerySettings&) = delete;

public:
    QuerySettings() = default;

    /**
     * Returns AllowedIndicesFilter for the query if it is set in the query settings, or
     * boost::none if it isn't.
     */
    boost::optional<AllowedIndicesFilter> getAllowedIndicesFilter(
        const CanonicalQuery::PlanCacheCommandKey& key) const;

    /**
     * Returns copies of all overrides for the collection.
     */
    std::vector<AllowedIndexEntry> getAllAllowedIndices() const;

    /**
     * Adds or replaces entry in query settings.
     * If existing entry is found for the same key, replaces it.
     */
    void setAllowedIndices(const CanonicalQuery& canonicalQuery,
                           const BSONObjSet& indexKeyPatterns,
                           const stdx::unordered_set<std::string>& indexNames);

    /**
     * Removes single entry from query settings. No effect if query shape is not found.
     */
    void removeAllowedIndices(const CanonicalQuery::PlanCacheCommandKey& key);

    /**
     * Clears all allowed indices from query settings.
     */
    void clearAllowedIndices();

private:
    // Allowed index entries owned here.
    using AllowedIndexEntryMap =
        stdx::unordered_map<CanonicalQuery::PlanCacheCommandKey, AllowedIndexEntry>;
    AllowedIndexEntryMap _allowedIndexEntryMap;

    /**
     * Protects data in query settings.
     */
    mutable Mutex _mutex = MONGO_MAKE_LATCH("QuerySettings::_mutex");
};

}  // namespace mongo
