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

#include <functional>
#include <string>

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

namespace repl {
class OplogEntryBase;
class DurableReplOperation;
}  // namespace repl

namespace idl {
namespace import {
class One_UUID;
}  // namespace import
}  // namespace idl

/**
 * A UUID is a 128-bit unique identifier, per RFC 4122, v4, using
 * a secure random number generator.
 */
class UUID {
    using UUIDStorage = std::array<unsigned char, 16>;

    // Make the IDL generated parser a friend
    friend class ConfigsvrShardCollectionResponse;
    friend class ConfigsvrCommitShardCollection;
    friend class ShardsvrShardCollectionResponse;
    friend class DatabaseVersion;
    friend class DbCheckOplogCollection;
    friend class EncryptionPlaceholder;
    friend class idl::import::One_UUID;
    friend class IndexBuildEntry;
    friend class KeyStoreRecord;
    friend class LogicalSessionId;
    friend class LogicalSessionToClient;
    friend class LogicalSessionIdToClient;
    friend class LogicalSessionFromClient;
    friend class ResolvedKeyId;
    friend class repl::OplogEntryBase;
    friend class repl::DurableReplOperation;
    friend class ResumeTokenInternal;
    friend class VoteCommitIndexBuild;

public:
    /**
     * The number of bytes contained in a UUID.
     */
    static constexpr int kNumBytes = sizeof(UUIDStorage);

    /**
     * Generate a new random v4 UUID per RFC 4122.
     */
    static UUID gen();

    /**
     * If the given string represents a valid UUID, constructs and returns the UUID,
     * otherwise returns an error.
     */
    static StatusWith<UUID> parse(const std::string& s);

    /**
     * If the given BSONElement represents a valid UUID, constructs and returns the UUID,
     * otherwise returns an error.
     */
    static StatusWith<UUID> parse(BSONElement from);

    /**
     * Parses a BSON document of the form { uuid: BinData(4, "...") }.
     *
     * For IDL.
     */
    static UUID parse(const BSONObj& obj);

    static UUID fromCDR(ConstDataRange cdr) {
        UUID uuid;
        invariant(cdr.length() == uuid._uuid.size());
        memcpy(uuid._uuid.data(), cdr.data(), uuid._uuid.size());
        return uuid;
    }

    /**
     * Returns whether this string represents a valid UUID.
     */
    static bool isUUIDString(const std::string& s);

    /**
     * Returns a ConstDataRange view of the UUID.
     */
    ConstDataRange toCDR() const {
        return ConstDataRange(_uuid);
    }

    /**
     * Appends to builder as BinData(4, "...") element with the given name.
     */
    void appendToBuilder(BSONObjBuilder* builder, StringData name) const;

    /**
     * Appends to array builder as BinData(4, "...").
     */
    void appendToArrayBuilder(BSONArrayBuilder* builder) const;

    /**
     * Returns a BSON object of the form { uuid: BinData(4, "...") }.
     */
    BSONObj toBSON() const;

    /**
     * Returns a string representation of this UUID, in hexadecimal,
     * as per RFC 4122:
     *
     * 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
     */
    std::string toString() const;

    inline bool operator==(const UUID& rhs) const {
        return !memcmp(&_uuid, &rhs._uuid, sizeof(_uuid));
    }

    inline bool operator!=(const UUID& rhs) const {
        return !(*this == rhs);
    }

    inline bool operator<(const UUID& rhs) const {
        return _uuid < rhs._uuid;
    }

    inline bool operator>(const UUID& rhs) const {
        return _uuid > rhs._uuid;
    }

    inline bool operator<=(const UUID& rhs) const {
        return _uuid <= rhs._uuid;
    }

    inline bool operator>=(const UUID& rhs) const {
        return _uuid >= rhs._uuid;
    }

    /**
     * Returns true only if the UUID is the RFC 4122 variant, v4 (random).
     */
    bool isRFC4122v4() const;

    /**
     * Custom hasher so UUIDs can be used in unordered data structures.
     *
     * ex: std::unordered_set<UUID, UUID::Hash> uuidSet;
     */
    struct Hash {
        std::size_t operator()(const UUID& uuid) const {
            uint32_t hash;
            MurmurHash3_x86_32(uuid._uuid.data(), UUID::kNumBytes, 0, &hash);
            return hash;
        }
    };

private:
    UUID(const UUIDStorage& uuid) : _uuid(uuid) {}

    /**
     *  Should never be used, as the resulting UUID will not be unique.
     *  The exception is in code generated by the IDL compiler, which itself ensures
     *  such an invalid value cannot propagate.
     */
    friend class LogicalSessionId;
    UUID() = default;

    UUIDStorage _uuid{};  // UUID in network byte order
};

inline std::ostream& operator<<(std::ostream& s, const UUID& uuid) {
    return (s << uuid.toString());
}

inline StringBuilder& operator<<(StringBuilder& s, const UUID& uuid) {
    return (s << uuid.toString());
}

/**
 * Supports use of UUID with the BSON macro:
 *     BSON("uuid" << uuid) -> { uuid: BinData(4, "...") }
 */
template <>
BSONObjBuilder& BSONObjBuilderValueStream::operator<<<UUID>(UUID value);

}  // namespace mongo
