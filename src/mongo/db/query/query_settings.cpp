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

using std::vector;

//
// HintOverride
//

AllowedIndices::AllowedIndices(const std::vector<BSONObj>& indexKeyPatterns) {
    for (std::vector<BSONObj>::const_iterator i = indexKeyPatterns.begin();
         i != indexKeyPatterns.end();
         ++i) {
        const BSONObj& indexKeyPattern = *i;
        this->indexKeyPatterns.push_back(indexKeyPattern.getOwned());
    }
}

AllowedIndices::~AllowedIndices() {}

//
// AllowedIndexEntry
//

AllowedIndexEntry::AllowedIndexEntry(const BSONObj& query,
                                     const BSONObj& sort,
                                     const BSONObj& projection,
                                     const BSONObj& collation,
                                     const std::vector<BSONObj>& indexKeyPatterns)
    : query(query.getOwned()),
      sort(sort.getOwned()),
      projection(projection.getOwned()),
      collation(collation.getOwned()) {
    for (std::vector<BSONObj>::const_iterator i = indexKeyPatterns.begin();
         i != indexKeyPatterns.end();
         ++i) {
        const BSONObj& indexKeyPattern = *i;
        this->indexKeyPatterns.push_back(indexKeyPattern.getOwned());
    }
}

AllowedIndexEntry::~AllowedIndexEntry() {}

AllowedIndexEntry* AllowedIndexEntry::clone() const {
    AllowedIndexEntry* entry =
        new AllowedIndexEntry(query, sort, projection, collation, indexKeyPatterns);
    return entry;
}

//
// QuerySettings
//

QuerySettings::QuerySettings() {}

QuerySettings::~QuerySettings() {
    _clear();
}

bool QuerySettings::getAllowedIndices(const PlanCacheKey& key,
                                      AllowedIndices** allowedIndicesOut) const {
    invariant(allowedIndicesOut);

    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    AllowedIndexEntryMap::const_iterator cacheIter = _allowedIndexEntryMap.find(key);

    // Nothing to do if key does not exist in query settings.
    if (cacheIter == _allowedIndexEntryMap.end()) {
        *allowedIndicesOut = NULL;
        return false;
    }

    AllowedIndexEntry* entry = cacheIter->second;

    // Create a AllowedIndices from entry.
    *allowedIndicesOut = new AllowedIndices(entry->indexKeyPatterns);

    return true;
}

std::vector<AllowedIndexEntry*> QuerySettings::getAllAllowedIndices() const {
    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    vector<AllowedIndexEntry*> entries;
    for (AllowedIndexEntryMap::const_iterator i = _allowedIndexEntryMap.begin();
         i != _allowedIndexEntryMap.end();
         ++i) {
        AllowedIndexEntry* entry = i->second;
        entries.push_back(entry->clone());
    }
    return entries;
}

void QuerySettings::setAllowedIndices(const CanonicalQuery& canonicalQuery,
                                      const PlanCacheKey& key,
                                      const std::vector<BSONObj>& indexes) {
    const QueryRequest& qr = canonicalQuery.getQueryRequest();
    const BSONObj& query = qr.getFilter();
    const BSONObj& sort = qr.getSort();
    const BSONObj& projection = qr.getProj();
    const BSONObj collation =
        canonicalQuery.getCollator() ? canonicalQuery.getCollator()->getSpec().toBSON() : BSONObj();
    AllowedIndexEntry* entry = new AllowedIndexEntry(query, sort, projection, collation, indexes);

    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    AllowedIndexEntryMap::iterator i = _allowedIndexEntryMap.find(key);
    // Replace existing entry.
    if (i != _allowedIndexEntryMap.end()) {
        AllowedIndexEntry* entry = i->second;
        delete entry;
    }
    _allowedIndexEntryMap[key] = entry;
}

void QuerySettings::removeAllowedIndices(const PlanCacheKey& key) {
    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    AllowedIndexEntryMap::iterator i = _allowedIndexEntryMap.find(key);

    // Nothing to do if key does not exist in query settings.
    if (i == _allowedIndexEntryMap.end()) {
        return;
    }

    // Free up resources and delete entry.
    AllowedIndexEntry* entry = i->second;
    _allowedIndexEntryMap.erase(i);
    delete entry;
}

void QuerySettings::clearAllowedIndices() {
    stdx::lock_guard<stdx::mutex> cacheLock(_mutex);
    _clear();
}

void QuerySettings::_clear() {
    for (AllowedIndexEntryMap::const_iterator i = _allowedIndexEntryMap.begin();
         i != _allowedIndexEntryMap.end();
         ++i) {
        AllowedIndexEntry* entry = i->second;
        delete entry;
    }
    _allowedIndexEntryMap.clear();
}

}  // namespace mongo
