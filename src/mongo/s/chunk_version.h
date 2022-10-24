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

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * The most-significant component of the shard versioning protocol (collection epoch/timestamp).
 */
class CollectionGeneration {
public:
    CollectionGeneration(OID epoch, Timestamp timestamp) : _epoch(epoch), _timestamp(timestamp) {}

    /**
     * Returns whether the combination of epoch/timestamp for two collections indicates that they
     * are the same collection for the purposes of sharding or not.
     *
     * Will throw if the combinations provided are illegal (for example matching timestamps, but
     * different epochs or vice-versa).
     */
    bool isSameCollection(const CollectionGeneration& other) const;

    std::string toString() const;

    // TODO: Do not add any new usages of these methods. Use isSameCollection instead.

    const OID& epoch() const {
        return _epoch;
    }

    const Timestamp& getTimestamp() const {
        return _timestamp;
    }

protected:
    static CollectionGeneration IGNORED() {
        CollectionGeneration gen{OID(), Timestamp()};
        gen._epoch.init(Date_t(), true);    // ignored OID is zero time, max machineId/inc
        gen._timestamp = Timestamp::max();  // ignored Timestamp is the largest timestamp
        return gen;
    }

    static CollectionGeneration UNSHARDED() {
        return CollectionGeneration{OID(), Timestamp()};
    }

    OID _epoch;
    Timestamp _timestamp;
};

/**
 * Reflects the placement information for a collection. An object of this class has no meaning on
 * its own without the Generation component above, that's why most of its methods are protected and
 * are exposed as semantic checks in ChunkVersion below.
 */
class CollectionPlacement {
public:
    CollectionPlacement(uint32_t major, uint32_t minor)
        : _combined(static_cast<uint64_t>(minor) | (static_cast<uint64_t>(major) << 32)) {}

    // TODO: Do not add any new usages of these methods. Use isSamePlacement instead.

    uint32_t majorVersion() const {
        return _combined >> 32;
    }

    uint32_t minorVersion() const {
        return _combined & 0xFFFFFFFF;
    }

protected:
    /**
     * Returns whether two collection placements are compatible with each other (meaning that they
     * refer to the same distribution of chunks across the cluster).
     */
    bool isSamePlacement(const CollectionPlacement& other) const {
        return majorVersion() == other.majorVersion();
    }

    // The combined major/minor version, which exists as subordinate to the collection generation
    uint64_t _combined;
};

/**
 * ChunkVersions consist of a major/minor version scoped to a version epoch
 *
 * Version configurations (format: major version, epoch):
 *
 * 1. (0, 0) - collection is dropped.
 * 2. (0, n), n > 0 - applicable only to shardVersion; shard has no chunk.
 * 3. (n, 0), n > 0 - invalid configuration.
 * 4. (n, m), n > 0, m > 0 - normal sharded collection version.
 */
class ChunkVersion : public CollectionGeneration, public CollectionPlacement {
public:
    /**
     * The name for the chunk version information field, which ddl operations use to send only
     * the placement information. String is shardVersion for compatibility with previous versions.
     */
    static constexpr StringData kChunkVersionField = "shardVersion"_sd;

    ChunkVersion(CollectionGeneration geneneration, CollectionPlacement placement)
        : CollectionGeneration(geneneration), CollectionPlacement(placement) {}

    ChunkVersion() : ChunkVersion({OID(), Timestamp()}, {0, 0}) {}

    /**
     * Indicates that the collection is not sharded.
     */
    static ChunkVersion UNSHARDED() {
        return ChunkVersion(CollectionGeneration::UNSHARDED(), {0, 0});
    }

    /**
     * Indicates that the shard version checking must be skipped.
     */
    static ChunkVersion IGNORED() {
        return ChunkVersion(CollectionGeneration::IGNORED(), {0, 0});
    }

    void incMajor() {
        uassert(
            31180,
            "The chunk major version has reached its maximum value. Manual intervention will be "
            "required before more chunk move, split, or merge operations are allowed.",
            majorVersion() != std::numeric_limits<uint32_t>::max());

        _combined = static_cast<uint64_t>(majorVersion() + 1) << 32;
    }

    void incMinor() {
        uassert(
            31181,
            "The chunk minor version has reached its maximum value. Manual intervention will be "
            "required before more chunk split or merge operations are allowed.",
            minorVersion() != std::numeric_limits<uint32_t>::max());

        _combined++;
    }

    // Note: this shouldn't be used as a substitute for version except in specific cases -
    // epochs make versions more complex
    unsigned long long toLong() const {
        return _combined;
    }

    bool isSet() const {
        return _combined > 0;
    }

    bool operator==(const ChunkVersion& otherVersion) const {
        return otherVersion.getTimestamp() == getTimestamp() && otherVersion._combined == _combined;
    }

    bool operator!=(const ChunkVersion& otherVersion) const {
        return !(otherVersion == *this);
    }

    // Can we write to this data and not have a problem?
    bool isWriteCompatibleWith(const ChunkVersion& other) const {
        return isSameCollection(other) && isSamePlacement(other);
    }

    // Unsharded timestamp cannot be compared with other timestamps
    bool isNotComparableWith(const ChunkVersion& other) const {
        return *this == UNSHARDED() || other == UNSHARDED() || *this == IGNORED() ||
            other == IGNORED();
    }

    /**
     * Returns true if both versions are comparable (i.e. neither version is UNSHARDED) and the
     * current version is older than the other one. Returns false otherwise.
     */
    bool isOlderThan(const ChunkVersion& otherVersion) const {
        if (isNotComparableWith(otherVersion))
            return false;

        if (getTimestamp() != otherVersion.getTimestamp())
            return getTimestamp() < otherVersion.getTimestamp();

        if (majorVersion() != otherVersion.majorVersion())
            return majorVersion() < otherVersion.majorVersion();

        return minorVersion() < otherVersion.minorVersion();
    }

    /**
     * Returns true if both versions are comparable (i.e. same epochs) and the current version is
     * older or equal than the other one. Returns false otherwise.
     */
    bool isOlderOrEqualThan(const ChunkVersion& otherVersion) const {
        return isOlderThan(otherVersion) || (*this == otherVersion);
    }

    static ChunkVersion parse(const BSONElement& element);
    void serialize(StringData field, BSONObjBuilder* builder) const;

    std::string toString() const;
};

inline std::ostream& operator<<(std::ostream& s, const ChunkVersion& v) {
    return s << v.toString();
}

inline StringBuilder& operator<<(StringBuilder& s, const ChunkVersion& v) {
    return s << v.toString();
}

}  // namespace mongo
