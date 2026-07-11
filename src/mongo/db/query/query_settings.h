// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
     * Returns AllowedIndicesFilter for the 'query' if it is set in the query settings, or
     * boost::none if it isn't.
     */
    boost::optional<AllowedIndicesFilter> getAllowedIndicesFilter(
        const CanonicalQuery& query) const;

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
    /**
     * Updates '_someAllowedIndexEntriesPresent' field state. Should be invoked when the mutex
     * protecting the index filters is held and just before releasing the mutex to ensure
     * '_someAllowedIndexEntriesPresent' is consistent with '_allowedIndexEntryMap'.
     */
    void _updateSomeAllowedIndexEntriesPresent();

    // Allowed index entries owned here.
    using AllowedIndexEntryMap =
        stdx::unordered_map<CanonicalQuery::PlanCacheCommandKey, AllowedIndexEntry>;
    AllowedIndexEntryMap _allowedIndexEntryMap;

    // Is 'true' if '_allowedIndexEntryMap' has at least one entry. It is used to avoid acquiring
    // the mutex in a typical scenario - when there are no index filters associated with the
    // collection.
    Atomic<bool> _someAllowedIndexEntriesPresent{false};

    /**
     * Protects data in query settings.
     */
    mutable std::mutex _mutex;
};

}  // namespace mongo
