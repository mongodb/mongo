// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"

#include <tuple>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

//
// AllowedIndicesFilter
//

AllowedIndicesFilter::AllowedIndicesFilter(const BSONObjSet& indexKeyPatterns,
                                           const stdx::unordered_set<std::string>& indexNames)
    : indexKeyPatterns(SimpleBSONObjComparator::kInstance.makeBSONObjSet()),
      indexNames(indexNames) {
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
                                     const stdx::unordered_set<std::string>& indexNames)
    : query(query.getOwned()),
      sort(sort.getOwned()),
      projection(projection.getOwned()),
      collation(collation.getOwned()),
      indexKeyPatterns(SimpleBSONObjComparator::kInstance.makeBSONObjSet()),
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
    const CanonicalQuery& canonicalQuery) const {
    // Fast-check if there is at least one allowed index in query settings.
    if (!_someAllowedIndexEntriesPresent.loadRelaxed()) {
        return {};
    }

    // Compute the key before entering the critical section.
    const CanonicalQuery::PlanCacheCommandKey key = canonicalQuery.encodeKeyForPlanCacheCommand();

    std::lock_guard<std::mutex> cacheLock(_mutex);
    AllowedIndexEntryMap::const_iterator cacheIter = _allowedIndexEntryMap.find(key);

    // Nothing to do if key does not exist in query settings.
    if (cacheIter == _allowedIndexEntryMap.end()) {
        return {};
    }

    return AllowedIndicesFilter(cacheIter->second.indexKeyPatterns, cacheIter->second.indexNames);
}

std::vector<AllowedIndexEntry> QuerySettings::getAllAllowedIndices() const {
    std::lock_guard<std::mutex> cacheLock(_mutex);
    std::vector<AllowedIndexEntry> entries;
    for (const auto& entryPair : _allowedIndexEntryMap) {
        entries.push_back(entryPair.second);
    }
    return entries;
}

void QuerySettings::setAllowedIndices(const CanonicalQuery& canonicalQuery,
                                      const BSONObjSet& indexKeyPatterns,
                                      const stdx::unordered_set<std::string>& indexNames) {
    const FindCommandRequest& findCommand = canonicalQuery.getFindCommandRequest();
    const BSONObj& query = findCommand.getFilter();
    const BSONObj& sort = findCommand.getSort();
    const BSONObj& projection = findCommand.getProjection();
    const auto key = canonicalQuery.encodeKeyForPlanCacheCommand();
    const BSONObj collation =
        canonicalQuery.getCollator() ? canonicalQuery.getCollator()->getSpec().toBSON() : BSONObj();

    std::lock_guard<std::mutex> cacheLock(_mutex);
    _allowedIndexEntryMap.erase(key);
    _allowedIndexEntryMap.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(query, sort, projection, collation, indexKeyPatterns, indexNames));
    _updateSomeAllowedIndexEntriesPresent();
}

void QuerySettings::removeAllowedIndices(const CanonicalQuery::PlanCacheCommandKey& key) {
    std::lock_guard<std::mutex> cacheLock(_mutex);
    AllowedIndexEntryMap::iterator i = _allowedIndexEntryMap.find(key);

    // Nothing to do if key does not exist in query settings.
    if (i == _allowedIndexEntryMap.end()) {
        return;
    }

    _allowedIndexEntryMap.erase(i);
    _updateSomeAllowedIndexEntriesPresent();
}

void QuerySettings::clearAllowedIndices() {
    std::lock_guard<std::mutex> cacheLock(_mutex);
    _allowedIndexEntryMap.clear();
    _updateSomeAllowedIndexEntriesPresent();
}

void QuerySettings::_updateSomeAllowedIndexEntriesPresent() {
    _someAllowedIndexEntriesPresent.store(!_allowedIndexEntryMap.empty());
}

}  // namespace mongo
