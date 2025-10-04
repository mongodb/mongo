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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <compare>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <string>

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
 * 2. (0, n), n > 0 - applicable only to shard placement version; shard has no chunk.
 * 3. (n, 0), n > 0 - invalid configuration.
 * 4. (n, m), n > 0, m > 0 - normal sharded collection placement version.
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
     * Indicates that placement version checking must be skipped.
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

    /**
     * Three-way comparison operator for ChunkVersion.
     * Returns:
     * - partial_ordering::unordered if versions are not comparable
     * - partial_ordering::less if this version is older than other
     * - partial_ordering::greater if this version is newer than other
     * - partial_ordering::equivalent if versions are equal
     *
     * Non-comparable versions (partial_ordering::unordered) include:
     * - UNSHARDED versions
     * - IGNORED versions
     * - Versions from the same collection generation where at least one has unset placement version
     * ({0,0})
     * Note: Versions with unset placement from different collection generations can be compared. *
     */
    std::partial_ordering operator<=>(const ChunkVersion& otherVersion) const {
        // Check for non-comparable versions (UNSHARDED, IGNORED)
        if (*this == UNSHARDED() || otherVersion == UNSHARDED() || *this == IGNORED() ||
            otherVersion == IGNORED()) {
            return std::partial_ordering::unordered;
        }

        // Check for unset placement versions from the same collection generation
        if ((!this->isSet() || !otherVersion.isSet()) && this->isSameCollection(otherVersion)) {
            return std::partial_ordering::unordered;
        }

        if (getTimestamp() != otherVersion.getTimestamp()) {
            return (getTimestamp() < otherVersion.getTimestamp()) ? std::partial_ordering::less
                                                                  : std::partial_ordering::greater;
        }

        if (majorVersion() != otherVersion.majorVersion()) {
            return (majorVersion() < otherVersion.majorVersion()) ? std::partial_ordering::less
                                                                  : std::partial_ordering::greater;
        }

        return minorVersion() <=> otherVersion.minorVersion();
    }

    // Delete unsafe comparison operators. When any operand results in
    // std::partial_ordering::unordered, these operators would return false,
    // which can lead to incorrect logic and potential bugs.
    bool operator<(const ChunkVersion& other) const = delete;
    bool operator>(const ChunkVersion& other) const = delete;
    bool operator<=(const ChunkVersion& other) const = delete;
    bool operator>=(const ChunkVersion& other) const = delete;

    // Can we write to this data and not have a problem?
    bool isWriteCompatibleWith(const ChunkVersion& other) const {
        return isSameCollection(other) && isSamePlacement(other);
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
