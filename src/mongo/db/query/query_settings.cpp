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

#include "mongo/db/query/query_settings.h"

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache.h"

namespace mongo {

//
// AllowedIndicesFilter
//

AllowedIndicesFilter::AllowedIndicesFilter(const BSONObjSet& indexKeyPatterns,
                                           const std::unordered_set<std::string>& indexNames)
    : indexNames(indexNames) {
    for (BSONObjSet::const_iterator i = indexKeyPatterns.begin(); i != indexKeyPatterns.end();
         ++i) {
        const BSONObj& indexKeyPattern = *i;
        this->indexKeyPatterns.insert(indexKeyPattern.getOwned());
    }
}

//
// AllowedIndexEntry
//

AllowedIndexEntry::AllowedIndexEntry(const BSONObj& query,
                                     const BSONObj& sort,
                                     const BSONObj& projection,
                                     const BSONObj& collation,
                                     const BSONObjSet& indexKeyPatterns,
                                     const std::unordered_set<std::string>& indexNames)
    : query(query.getOwned()),
      sort(sort.getOwned()),
      projection(projection.getOwned()),
      collation(collation.getOwned()),
      indexNames(indexNames) {
    for (BSONObjSet::const_iterator i = indexKeyPatterns.begin(); i != indexKeyPatterns.end();
         ++i) {
        const BSONObj& indexKeyPattern = *i;
        this->indexKeyPatterns.insert(indexKeyPattern.getOwned());
    }
}

//
// QuerySettings
//

boost::optional<AllowedIndicesFilter> QuerySettings::getAllowedIndicesFilter(
    const PlanCacheKey& key) const {
    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    AllowedIndexEntryMap::const_iterator cacheIter = _allowedIndexEntryMap.find(key);

    // Nothing to do if key does not exist in query settings.
    if (cacheIter == _allowedIndexEntryMap.end()) {
        return {};
    }

    return AllowedIndicesFilter(cacheIter->second.indexKeyPatterns, cacheIter->second.indexNames);
}

std::vector<AllowedIndexEntry> QuerySettings::getAllAllowedIndices() const {
    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    std::vector<AllowedIndexEntry> entries;
    for (const auto& entryPair : _allowedIndexEntryMap) {
        entries.push_back(entryPair.second);
    }
    return entries;
}

void QuerySettings::setAllowedIndices(const CanonicalQuery& canonicalQuery,
                                      const PlanCacheKey& key,
                                      const BSONObjSet& indexKeyPatterns,
                                      const std::unordered_set<std::string>& indexNames) {
    const QueryRequest& qr = canonicalQuery.getQueryRequest();
    const BSONObj& query = qr.getFilter();
    const BSONObj& sort = qr.getSort();
    const BSONObj& projection = qr.getProj();
    const BSONObj collation =
        canonicalQuery.getCollator() ? canonicalQuery.getCollator()->getSpec().toBSON() : BSONObj();

    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    _allowedIndexEntryMap.erase(key);
    _allowedIndexEntryMap.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(query, sort, projection, collation, indexKeyPatterns, indexNames));
}

void QuerySettings::removeAllowedIndices(const PlanCacheKey& key) {
    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    AllowedIndexEntryMap::iterator i = _allowedIndexEntryMap.find(key);

    // Nothing to do if key does not exist in query settings.
    if (i == _allowedIndexEntryMap.end()) {
        return;
    }

    _allowedIndexEntryMap.erase(i);
}

void QuerySettings::clearAllowedIndices() {
    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    _allowedIndexEntryMap.clear();
}

}  // namespace mongo
