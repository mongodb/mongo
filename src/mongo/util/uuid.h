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

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iosfwd>
#include <string>

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {

/**
 * A UUID is a 128-bit unique identifier, per RFC 4122, v4, using
 * a secure random number generator.
 */
class UUID {
public:
    /**
     * The number of bytes contained in a UUID.
     */
    static constexpr int kNumBytes = 16;

    /**
     * Generate a new random v4 UUID per RFC 4122.
     */
    static UUID gen();

    /**
     * If the given string represents a valid UUID, constructs and returns the UUID,
     * otherwise returns an error.
     */
    static StatusWith<UUID> parse(StringData s);

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
        UUID uuid{UUIDStorage{}};
        // Allow empty CDRs to generate empty UUIDs.
        if (cdr.length() > 0) {
            invariant(cdr.length() == uuid._uuid.size());
            memcpy(uuid._uuid.data(), cdr.data(), uuid._uuid.size());
        }

        return uuid;
    }

    /**
     * Returns whether this string represents a valid UUID.
     */
    static bool isUUIDString(StringData s);

    /*
     * Return the underlying 128-bit array.
     */
    std::array<unsigned char, 16> data() const {
        return _uuid;
    }

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

    ConstDataRange asDataRange() const {
        return ConstDataRange(_uuid.data(), _uuid.size());
    }

    inline int compare(const UUID& rhs) const {
        return memcmp(&_uuid, &rhs._uuid, sizeof(_uuid));
    }

    inline bool operator==(const UUID& rhs) const {
        return !compare(rhs);
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

    template <typename H>
    friend H AbslHashValue(H h, const UUID& uuid) {
        return H::combine(std::move(h), uuid._uuid);
    }

    /**
     * Returns true only if the UUID is the RFC 4122 variant, v4 (random).
     */
    bool isRFC4122v4() const;

    /**
     * Custom hasher so UUIDs can be used in unordered data structures. Uses the first four bytes
     * of the UUID itself as the hash, since these are already randomly generated.
     * e.g. std::unordered_set<UUID, UUID::Hash> uuidSet;
     */
    struct Hash {
        std::size_t operator()(const UUID& uuid) const {
            return uuid.toCDR()
                .read<BigEndian<uint32_t>>();  // BigEndian because UUID is in network order
        }
    };

    friend auto logAttrs(const UUID& uuid) {
        return "uuid"_attr = uuid;
    }

private:
    using UUIDStorage = std::array<unsigned char, kNumBytes>;

    UUID(const UUIDStorage& uuid) : _uuid(uuid) {}

    UUIDStorage _uuid{};  // UUID in network byte order
};

/** Allow IDL-generated parsers to define uninitialized UUID objects. */
inline auto idlPreparsedValue(stdx::type_identity<UUID>) {
    return UUID::fromCDR(std::array<unsigned char, 16>{});
}

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
