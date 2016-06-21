/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * Holds allowed indices.
 */
class AllowedIndices {
private:
    MONGO_DISALLOW_COPYING(AllowedIndices);

public:
    AllowedIndices(const std::vector<BSONObj>& indexKeyPatterns);
    ~AllowedIndices();

    // These are the index key patterns that
    // we will use to override the indexes retrieved from
    // the index catalog.
    std::vector<BSONObj> indexKeyPatterns;
};

/**
 * Value type for query settings.
 * Holds:
 *     query shape (query, sort, projection, collation)
 *     vector of index specs
 */
class AllowedIndexEntry {
private:
    MONGO_DISALLOW_COPYING(AllowedIndexEntry);

public:
    AllowedIndexEntry(const BSONObj& query,
                      const BSONObj& sort,
                      const BSONObj& projection,
                      const BSONObj& collation,
                      const std::vector<BSONObj>& indexKeyPatterns);
    ~AllowedIndexEntry();
    AllowedIndexEntry* clone() const;

    // query, sort, projection, and collation collectively represent the query shape that we are
    // storing hint overrides for.
    BSONObj query;
    BSONObj sort;
    BSONObj projection;
    BSONObj collation;

    // These are the index key patterns that
    // we will use to override the indexes retrieved from
    // the index catalog.
    std::vector<BSONObj> indexKeyPatterns;
};

/**
 * Holds the index filters in a collection.
 */
class QuerySettings {
private:
    MONGO_DISALLOW_COPYING(QuerySettings);

public:
    QuerySettings();

    ~QuerySettings();

    /**
     * Returns true and fills out allowedIndicesOut if a hint is set in the query settings
     * for the query.
     * Returns false and sets allowedIndicesOut to NULL otherwise.
     * Caller owns AllowedIndices.
     */
    bool getAllowedIndices(const PlanCacheKey& query, AllowedIndices** allowedIndicesOut) const;

    /**
     * Returns copies all overrides for the collection..
     * Caller owns overrides in vector.
     */
    std::vector<AllowedIndexEntry*> getAllAllowedIndices() const;

    /**
     * Adds or replaces entry in query settings.
     * If existing entry is found for the same key,
     * frees resources for existing entry before replacing.
     */
    void setAllowedIndices(const CanonicalQuery& canonicalQuery,
                           const PlanCacheKey& key,
                           const std::vector<BSONObj>& indexes);

    /**
     * Removes single entry from query settings. No effect if query shape is not found.
     */
    void removeAllowedIndices(const PlanCacheKey& canonicalQuery);

    /**
     * Clears all allowed indices from query settings.
     */
    void clearAllowedIndices();

private:
    /**
     * Clears entries without acquiring mutex.
     */
    void _clear();

    typedef unordered_map<PlanCacheKey, AllowedIndexEntry*> AllowedIndexEntryMap;
    AllowedIndexEntryMap _allowedIndexEntryMap;

    /**
     * Protects data in query settings.
     */
    mutable stdx::mutex _mutex;
};

}  // namespace mongo
