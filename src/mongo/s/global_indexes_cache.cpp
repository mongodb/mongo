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

#include "mongo/s/global_indexes_cache.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

AtomicWord<uint64_t> ComparableIndexVersion::_epochDisambiguatingSequenceNumSource{1ULL};
AtomicWord<uint64_t> ComparableIndexVersion::_forcedRefreshSequenceNumSource{1ULL};

ComparableIndexVersion ComparableIndexVersion::makeComparableIndexVersion(
    const CollectionIndexes& version) {
    return ComparableIndexVersion(_forcedRefreshSequenceNumSource.load(),
                                  version,
                                  _epochDisambiguatingSequenceNumSource.fetchAndAdd(1));
}

ComparableIndexVersion ComparableIndexVersion::makeComparableIndexVersionForForcedRefresh() {
    return ComparableIndexVersion(_forcedRefreshSequenceNumSource.addAndFetch(2) - 1,
                                  boost::none,
                                  _epochDisambiguatingSequenceNumSource.fetchAndAdd(1));
}

void ComparableIndexVersion::setCollectionIndexes(const CollectionIndexes& version) {
    _indexVersion = version;
}

std::string ComparableIndexVersion::toString() const {
    BSONObjBuilder builder;
    if (_indexVersion)
        builder.append("collectionIndexes"_sd, _indexVersion->toBSONForLogging());
    else
        builder.append("collectionIndexes"_sd, "None");

    builder.append("forcedRefreshSequenceNum"_sd, static_cast<int64_t>(_forcedRefreshSequenceNum));
    builder.append("epochDisambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_epochDisambiguatingSequenceNum));

    return builder.obj().toString();
}

bool ComparableIndexVersion::operator==(const ComparableIndexVersion& other) const {
    if (_forcedRefreshSequenceNum != other._forcedRefreshSequenceNum)
        return false;  // Values with different forced refresh sequence numbers are always
                       // considered different, regardless of the underlying versions
    if (_forcedRefreshSequenceNum == 0)
        return true;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                      // they are always equal

    // Relying on the boost::optional<CollectionIndexes>::operator== comparison
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
    if (_indexVersion.has_value() != other._indexVersion.has_value())
        return _epochDisambiguatingSequenceNum <
            other._epochDisambiguatingSequenceNum;  // One side is not initialised, but the other
                                                    // is, which can only happen if one side is
                                                    // ForForcedRefresh and the other is made from
                                                    // makeComparableIndexVersion. In this case, use
                                                    // the _epochDisambiguatingSequenceNum to see
                                                    // which one is more recent.
    if (!_indexVersion.has_value())
        return _epochDisambiguatingSequenceNum <
            other._epochDisambiguatingSequenceNum;  // Both sides are not initialised, which can
                                                    // only happen if both were created from
                                                    // ForForcedRefresh. In this case, use the
                                                    // _epochDisambiguatingSequenceNum to see which
                                                    // one is more recent.

    if (_indexVersion->getTimestamp() == other._indexVersion->getTimestamp()) {
        if (!_indexVersion->isSet() && !other._indexVersion->isSet()) {
            return false;
        } else if (_indexVersion->isSet() && other._indexVersion->isSet()) {
            return _indexVersion->indexVersion() < other._indexVersion->indexVersion();
        }
    } else if (_indexVersion->isSet() && other._indexVersion->isSet()) {
        return _indexVersion->getTimestamp() < other._indexVersion->getTimestamp();
    }
    return _epochDisambiguatingSequenceNum < other._epochDisambiguatingSequenceNum;
}

}  // namespace mongo
