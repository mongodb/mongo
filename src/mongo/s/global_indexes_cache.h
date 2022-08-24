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

#pragma once

#include "mongo/s/index_version.h"

namespace mongo {

/**
 * Constructed to be used exclusively by the CatalogCache as a vector clock (Time) to drive
 * IndexCache's lookups.
 *
 * The CollectionIndexes class contains a timestamp for the collection generation which resets to 0
 * if the collection is dropped, in which case the versions cannot be compared.
 *
 * This class wraps a CollectionIndexes object with a node-local sequence number
 * (_epochDisambiguatingSequenceNum) that allows the comparision.
 *
 */
class ComparableIndexVersion {
public:
    /**
     * Creates a ComparableIndexVersion that wraps the given CollectionIndexes.
     * Each object created through this method will have a local sequence number greater than the
     * previously created ones.
     */
    static ComparableIndexVersion makeComparableIndexVersion(const CollectionIndexes& version);

    /**
     * Creates a new instance which will artificially be greater than any previously created
     * ComparableIndexVersion and smaller than any instance created afterwards. Used as means to
     * cause the collections cache to attempt a refresh in situations where causal consistency
     * cannot be inferred.
     */
    static ComparableIndexVersion makeComparableIndexVersionForForcedRefresh();

    /**
     * Empty constructor needed by the ReadThroughCache.
     *
     * Instances created through this constructor will be always less then the ones created through
     * the two static constructors, but they do not carry any meaningful value and can only be used
     * for comparison purposes.
     */
    ComparableIndexVersion() = default;

    std::string toString() const;

    bool operator==(const ComparableIndexVersion& other) const;

    bool operator!=(const ComparableIndexVersion& other) const {
        return !(*this == other);
    }

    /**
     * In case the two compared instances have different generations, the most recently created one
     * will be greater, otherwise the comparision will be driven by the index versions of the
     * underlying CollectionIndexes.
     */
    bool operator<(const ComparableIndexVersion& other) const;

    bool operator>(const ComparableIndexVersion& other) const {
        return other < *this;
    }

    bool operator<=(const ComparableIndexVersion& other) const {
        return !(*this > other);
    }

    bool operator>=(const ComparableIndexVersion& other) const {
        return !(*this < other);
    }

private:
    friend class CatalogCache;

    static AtomicWord<uint64_t> _epochDisambiguatingSequenceNumSource;
    static AtomicWord<uint64_t> _forcedRefreshSequenceNumSource;

    ComparableIndexVersion(uint64_t forcedRefreshSequenceNum,
                           boost::optional<CollectionIndexes> version,
                           uint64_t epochDisambiguatingSequenceNum)
        : _forcedRefreshSequenceNum(forcedRefreshSequenceNum),
          _indexVersion(std::move(version)),
          _epochDisambiguatingSequenceNum(epochDisambiguatingSequenceNum) {}

    void setCollectionIndexes(const CollectionIndexes& version);

    uint64_t _forcedRefreshSequenceNum{0};

    boost::optional<CollectionIndexes> _indexVersion;

    // Locally incremented sequence number that allows to compare two colection versions with
    // different generations. Each new ComparableIndexVersion will have a greater sequence number
    // than the ones created before.
    uint64_t _epochDisambiguatingSequenceNum{0};
};

}  // namespace mongo
