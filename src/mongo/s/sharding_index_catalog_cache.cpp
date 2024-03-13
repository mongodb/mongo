/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/sharding_index_catalog_cache.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstdint>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

bool ShardingIndexesCatalogCache::empty() const {
    return _indexes.empty();
}

CollectionIndexes ShardingIndexesCatalogCache::getCollectionIndexes() const {
    return _collectionIndexes;
}

size_t ShardingIndexesCatalogCache::numIndexes() const {
    return _indexes.size();
}

bool ShardingIndexesCatalogCache::contains(StringData name) const {
    return _indexes.contains(name);
}

void ShardingIndexesCatalogCache::add(const IndexCatalogType& index,
                                      const CollectionIndexes& collectionIndexes) {
    tassert(7019900,
            str::stream() << "Cannot add global index with different uuid than is in the "
                             "ShardingIndexesCatalogCache.",
            collectionIndexes.uuid() == _collectionIndexes.uuid());
    _collectionIndexes = collectionIndexes;
    _indexes.emplace(index.getName(), index);
}

void ShardingIndexesCatalogCache::remove(StringData name,
                                         const CollectionIndexes& collectionIndexes) {
    tassert(7019901,
            str::stream() << "Cannot remove global index with different uuid than is in the "
                             "ShardingIndexesCatalogCache.",
            collectionIndexes.uuid() == _collectionIndexes.uuid());
    _collectionIndexes = collectionIndexes;
    _indexes.erase(name);
}

AtomicWord<uint64_t> ComparableIndexVersion::_disambiguatingSequenceNumSource{1ULL};
AtomicWord<uint64_t> ComparableIndexVersion::_forcedRefreshSequenceNumSource{1ULL};

ComparableIndexVersion ComparableIndexVersion::makeComparableIndexVersion(
    const boost::optional<Timestamp>& version) {
    return ComparableIndexVersion(_forcedRefreshSequenceNumSource.load(),
                                  version,
                                  _disambiguatingSequenceNumSource.fetchAndAdd(1));
}

ComparableIndexVersion ComparableIndexVersion::makeComparableIndexVersionForForcedRefresh() {
    return ComparableIndexVersion(_forcedRefreshSequenceNumSource.addAndFetch(2) - 1,
                                  boost::none,
                                  _disambiguatingSequenceNumSource.fetchAndAdd(1));
}

void ComparableIndexVersion::setCollectionIndexes(const boost::optional<Timestamp>& version) {
    _indexVersion = version;
}

std::string ComparableIndexVersion::toString() const {
    BSONObjBuilder builder;
    if (_indexVersion)
        builder.append("collectionIndexes"_sd, _indexVersion->toString());
    else
        builder.append("collectionIndexes"_sd, "None");

    builder.append("forcedRefreshSequenceNum"_sd, static_cast<int64_t>(_forcedRefreshSequenceNum));
    builder.append("disambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_disambiguatingSequenceNum));

    return builder.obj().toString();
}

bool ComparableIndexVersion::operator==(const ComparableIndexVersion& other) const {
    if (_forcedRefreshSequenceNum != other._forcedRefreshSequenceNum)
        return false;  // Values with different forced refresh sequence numbers are always
                       // considered different, regardless of the underlying versions
    if (_forcedRefreshSequenceNum == 0)
        return true;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                      // they are always equal
    // Relying on the boost::optional<Timestamp>::operator== comparison
    return _indexVersion == other._indexVersion;
}

bool ComparableIndexVersion::operator<(const ComparableIndexVersion& other) const {
    if (_forcedRefreshSequenceNum < other._forcedRefreshSequenceNum)
        return true;  // Values with different forced refresh sequence numbers are always considered
                      // different, regardless of the underlying versions
    if (_forcedRefreshSequenceNum > other._forcedRefreshSequenceNum)
        return false;  // Values with different forced refresh sequence numbers are always
                       // considered different, regardless of the underlying versions
    if (_forcedRefreshSequenceNum == 0)
        return false;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                       // they are always equal

    if (_indexVersion.is_initialized() != other._indexVersion.is_initialized())
        return _disambiguatingSequenceNum <
            other._disambiguatingSequenceNum;  // If one value is set and the other is not, we must
                                               // rely on the disambiguating sequence number

    return _indexVersion < other._indexVersion;
}

}  // namespace mongo
