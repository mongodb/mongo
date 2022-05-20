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

protected:
    OID _epoch;
    Timestamp _timestamp;
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
 *
 * TODO (SERVER-65530): Get rid of all the legacy format parsers/serialisers
 */
class ChunkVersion : public CollectionGeneration {
public:
    /**
     * The name for the shard version information field, which shard-aware commands should include
     * if they want to convey shard version.
     */
    static constexpr StringData kShardVersionField = "shardVersion"_sd;

    ChunkVersion(uint32_t major, uint32_t minor, const OID& epoch, const Timestamp& timestamp)
        : CollectionGeneration(epoch, timestamp),
          _combined(static_cast<uint64_t>(minor) | (static_cast<uint64_t>(major) << 32)) {}

    ChunkVersion() : ChunkVersion(0, 0, OID(), Timestamp()) {}

    /**
     * Allow parsing a chunk version with the following formats:
     *  {<field>:(major, minor), <fieldEpoch>:epoch, <fieldTimestmap>:timestamp}
     *  {<field>: {t:timestamp, e:epoch, v:(major, minor) }}
     * TODO SERVER-63403: remove this function and only parse the new format.
     */
    static ChunkVersion fromBSONLegacyOrNewerFormat(const BSONObj& obj, StringData field = "");

    /**
     * Allow parsing a chunk version with the following formats:
     *  [major, minor, epoch, <optional canThrowSSVOnIgnored>, timestamp]
     *  {0:major, 1:minor, 2:epoch, 3:<optional canThrowSSVOnIgnored>, 4:timestamp}
     *  {t:timestamp, e:epoch, v:(major, minor)}
     * TODO SERVER-63403: remove this function and only parse the new format.
     */
    static ChunkVersion fromBSONPositionalOrNewerFormat(const BSONElement& element);

    /**
     * Indicates that the collection is not sharded.
     */
    static ChunkVersion UNSHARDED() {
        return ChunkVersion();
    }

    /**
     * Indicates that the shard version checking must be skipped.
     */
    static ChunkVersion IGNORED() {
        ChunkVersion version;
        version._epoch.init(Date_t(), true);    // ignored OID is zero time, max machineId/inc
        version._timestamp = Timestamp::max();  // ignored Timestamp is the largest timestamp
        return version;
    }

    static bool isIgnoredVersion(const ChunkVersion& version) {
        return version.majorVersion() == 0 && version.minorVersion() == 0 &&
            version.getTimestamp() == IGNORED().getTimestamp();
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

    uint32_t majorVersion() const {
        return _combined >> 32;
    }

    uint32_t minorVersion() const {
        return _combined & 0xFFFFFFFF;
    }

    const OID& epoch() const {
        return _epoch;
    }

    const Timestamp& getTimestamp() const {
        return _timestamp;
    }

    bool operator==(const ChunkVersion& otherVersion) const {
        return otherVersion.getTimestamp() == getTimestamp() && otherVersion._combined == _combined;
    }

    bool operator!=(const ChunkVersion& otherVersion) const {
        return !(otherVersion == *this);
    }

    // Can we write to this data and not have a problem?
    bool isWriteCompatibleWith(const ChunkVersion& other) const {
        return isSameCollection(other) && majorVersion() == other.majorVersion();
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
    void serializeToBSON(StringData field, BSONObjBuilder* builder) const;

    /**
     * NOTE: This format is being phased out. Use serializeToBSON instead.
     *
     * Serializes the version held by this object to 'out' in the legacy form:
     *  { ..., <field>: [ <combined major/minor> ],
     *         <field>Epoch: [ <OID epoch> ],
     *         <field>Timestamp: [ <Timestamp> ] ... }
     *  or
     *  { ..., <field> : {t: <Timestamp>, e: <OID>, v: <major/minor>}}.
     *
     * Depending on the FCV version
     */
    void appendLegacyWithField(BSONObjBuilder* out, StringData field) const;

    std::string toString() const;

    // Methods that are here for the purposes of parsing of ShardCollectionType only
    static ChunkVersion parseMajorMinorVersionOnlyFromShardCollectionType(
        const BSONElement& element);
    void serialiseMajorMinorVersionOnlyForShardCollectionType(StringData field,
                                                              BSONObjBuilder* builder) const;

private:
    // The following static functions will be deprecated. Only one function should be used to parse
    // ChunkVersion and is fromBSON.
    /**
     * The method below parse the "positional" formats of:
     *
     *  [major, minor, epoch, <optional canThrowSSVOnIgnored> timestamp]
     *      OR
     *  {0: major, 1:minor, 2:epoch, 3:<optional canThrowSSVOnIgnored>, 4:timestamp}
     *
     * The latter format was introduced by mistake in 4.4 and is no longer generated from 5.3
     * onwards, but it is backwards compatible with the 5.2 and older binaries.
     */
    static ChunkVersion _parseArrayOrObjectPositionalFormat(const BSONObj& obj);

    /**
     * Parses the BSON formatted by appendLegacyWithField. If the field is missing, returns
     * 'NoSuchKey', otherwise if the field is not properly formatted can return any relevant parsing
     * error (BadValue, TypeMismatch, etc).
     */
    static StatusWith<ChunkVersion> _parseLegacyWithField(const BSONObj& obj, StringData field);

private:
    // The combined major/minor version, which exists as subordinate to the collection generation
    uint64_t _combined;
};

inline std::ostream& operator<<(std::ostream& s, const ChunkVersion& v) {
    return s << v.toString();
}

inline StringBuilder& operator<<(StringBuilder& s, const ChunkVersion& v) {
    return s << v.toString();
}

}  // namespace mongo
