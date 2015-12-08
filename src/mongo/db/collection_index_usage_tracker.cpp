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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"

namespace mongo {

CollectionIndexUsageTracker::CollectionIndexUsageTracker(ClockSource* clockSource)
    : _clockSource(clockSource) {
    invariant(_clockSource);
}

void CollectionIndexUsageTracker::recordIndexAccess(StringData indexName) {
    invariant(!indexName.empty());
    dassert(_indexUsageMap.find(indexName) != _indexUsageMap.end());

    _indexUsageMap[indexName].accesses.fetchAndAdd(1);
}

void CollectionIndexUsageTracker::registerIndex(StringData indexName, const BSONObj& indexKey) {
    invariant(!indexName.empty());
    dassert(_indexUsageMap.find(indexName) == _indexUsageMap.end());

    // Create map entry.
    _indexUsageMap[indexName] = IndexUsageStats(_clockSource->now(), indexKey);
}

void CollectionIndexUsageTracker::unregisterIndex(StringData indexName) {
    invariant(!indexName.empty());

    _indexUsageMap.erase(indexName);
}

CollectionIndexUsageMap CollectionIndexUsageTracker::getUsageStats() const {
    return _indexUsageMap;
}

}  // namespace mongo
