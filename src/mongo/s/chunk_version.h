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
 * ChunkVersions consist of a major/minor version scoped to a version epoch
 *
 * Version configurations (format: major version, epoch):
 *
 * 1. (0, 0) - collection is dropped.
 * 2. (0, n), n > 0 - applicable only to shardVersion; shard has no chunk.
 * 3. (n, 0), n > 0 - invalid configuration.
 * 4. (n, m), n > 0, m > 0 - normal sharded collection version.
 *
 * TODO: This is a "manual type" but, even so, still needs to comform to what's
 * expected from types.
 */
struct ChunkVersion {
public:
    /**
     * The name for the shard version information field, which shard-aware commands should include
     * if they want to convey shard version.
     */
    static constexpr StringData kShardVersionField = "shardVersion"_sd;

    ChunkVersion() : _combined(0), _epoch(OID()) {}

    ChunkVersion(uint32_t major, uint32_t minor, const OID& epoch)
        : _combined(static_cast<uint64_t>(minor) | (static_cast<uint64_t>(major) << 32)),
          _epoch(epoch) {}

    static StatusWith<ChunkVersion> parseFromCommand(const BSONObj& obj) {
        return parseWithField(obj, kShardVersionField);
    }

    /**
     * Parses the BSON formatted by appendWithField. If the field is missing, returns 'NoSuchKey',
     * otherwise if the field is not properly formatted can return any relevant parsing error
     * (BadValue, TypeMismatch, etc).
     */
    static StatusWith<ChunkVersion> parseWithField(const BSONObj& obj, StringData field);

    /**
     * Parses 'obj', which is expected to have two elements: the timestamp and the object id. The
     * field names don't matter, so 'obj' can be a BSONArray.
     */
    static StatusWith<ChunkVersion> fromBSON(const BSONObj& obj);

    /**
     * A throwing version of 'fromBSON'.
     */
    static ChunkVersion fromBSONThrowing(const BSONObj& obj) {
        return uassertStatusOK(fromBSON(obj));
    }

    /**
     * NOTE: This format should not be used. Use fromBSONThrowing instead.
     *
     * A throwing version of 'parseLegacyWithField' to resolve a compatibility issue with the
     * ShardCollectionType IDL type.
     */
    static ChunkVersion legacyFromBSONThrowing(const BSONElement& element) {
        return uassertStatusOK(parseLegacyWithField(element.wrap(), element.fieldNameStringData()));
    }

    /**
     * NOTE: This format is being phased out. Use parseWithField instead.
     *
     * Parses the BSON formatted by appendLegacyWithField. If the field is missing, returns
     * 'NoSuchKey', otherwise if the field is not properly formatted can return any relevant parsing
     * error (BadValue, TypeMismatch, etc).
     */
    static StatusWith<ChunkVersion> parseLegacyWithField(const BSONObj& obj, StringData field);

    /**
     * Indicates a dropped collection. All components are zeroes (OID is zero time, zero
     * machineId/inc).
     */
    static ChunkVersion DROPPED() {
        return ChunkVersion(0, 0, OID());
    }

    /**
     * Indicates that the collection is not sharded. Same as DROPPED.
     */
    static ChunkVersion UNSHARDED() {
        return ChunkVersion(0, 0, OID());
    }

    /**
     * Indicates that the shard version checking must be skipped.
     */
    static ChunkVersion IGNORED() {
        ChunkVersion version = ChunkVersion();
        version._epoch.init(Date_t(), true);  // ignored OID is zero time, max machineId/inc
        return version;
    }

    static bool isIgnoredVersion(const ChunkVersion& version) {
        return version.majorVersion() == 0 && version.minorVersion() == 0 &&
            version.epoch() == IGNORED().epoch();
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

    //
    // Explicit comparison operators - versions with epochs have non-trivial comparisons.
    // > < operators do not check epoch cases.  Generally if using == we need to handle
    // more complex cases.
    //

    bool operator==(const ChunkVersion& otherVersion) const {
        return otherVersion.epoch() == epoch() && otherVersion._combined == _combined;
    }

    bool operator!=(const ChunkVersion& otherVersion) const {
        return !(otherVersion == *this);
    }

    bool operator>(const ChunkVersion& otherVersion) const {
        return this->_combined > otherVersion._combined;
    }

    bool operator>=(const ChunkVersion& otherVersion) const {
        return this->_combined >= otherVersion._combined;
    }

    bool operator<(const ChunkVersion& otherVersion) const {
        return this->_combined < otherVersion._combined;
    }

    // Can we write to this data and not have a problem?
    bool isWriteCompatibleWith(const ChunkVersion& other) const {
        return epoch() == other.epoch() && majorVersion() == other.majorVersion();
    }

    /**
     * Returns true if this version is (strictly) in the same epoch as the other version and
     * this version is older.  Returns false if we're not sure because the epochs are different
     * or if this version is newer.
     */
    bool isOlderThan(const ChunkVersion& otherVersion) const {
        if (otherVersion._epoch != _epoch)
            return false;

        if (majorVersion() != otherVersion.majorVersion())
            return majorVersion() < otherVersion.majorVersion();

        return minorVersion() < otherVersion.minorVersion();
    }

    void appendToCommand(BSONObjBuilder* out) const {
        appendWithField(out, kShardVersionField);
    }

    /**
     * Serializes the version held by this object to 'out' in the form:
     *  { ..., <field>: [ <combined major/minor>, <OID epoch> ], ... }.
     */
    void appendWithField(BSONObjBuilder* out, StringData field) const;

    /**
     * NOTE: This format is being phased out. Use appendWithField instead.
     *
     * Serializes the version held by this object to 'out' in the legacy form:
     *  { ..., <field>: [ <combined major/minor> ], <field>Epoch: [ <OID epoch> ], ... }
     */
    void appendLegacyWithField(BSONObjBuilder* out, StringData field) const;

    BSONObj toBSON() const;

    /**
     * NOTE: This format serializes chunk version as a timestamp (without the epoch) for
     * legacy reasons.
     */
    void legacyToBSON(StringData field, BSONObjBuilder* builder) const;
    std::string toString() const;

private:
    uint64_t _combined;
    OID _epoch;
};

inline std::ostream& operator<<(std::ostream& s, const ChunkVersion& v) {
    s << v.toString();
    return s;
}

}  // namespace mongo
