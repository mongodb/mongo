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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <new>
#include <ostream>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 *  Representation of a tenant identifier.
 */
class TenantId {
public:
    static const boost::optional<TenantId>& systemTenantId() {
        static StaticImmortal<boost::optional<TenantId>> systemTenantId{};
        return *systemTenantId;
    }

    /**
     * Parse a tenantId from a StringData. This method asserts if the tenantId is empty or not in an
     * OID format.
     * Returns a TenantId object from the parsed string.
     */
    static TenantId parseFromString(StringData tenantId);

    explicit TenantId(const OID& oid) : _oid(oid) {}

    TenantId() = delete;

    std::string toString() const {
        return _oid.toString();
    }

    /**
     * Returns -1, 0, or 1 if 'this' is less, equal, or greater than 'other' in
     * lexicographical order.
     */
    int compare(const TenantId& other) const {
        return _oid.compare(other._oid);
    }

    std::size_t hash() const {
        return OID::Hasher()(_oid);
    }

    /**
     * Functor compatible with std::hash for std::unordered_{map,set}
     */
    struct Hasher {
        std::size_t operator()(const TenantId& tenantId) const {
            return tenantId.hash();
        }
    };

    /**
     * Hash function compatible with absl::Hash for absl::unordered_{map,set}
     */
    template <typename H>
    friend H AbslHashValue(H h, const TenantId& tenantId) {
        return H::combine(std::move(h), tenantId._oid);
    }

    /**
     * Hash function compatible with absl::Hash with an absl::unordered_{map,set} keyed with
     * boost::optional<TenantId>.
     */
    template <typename H>
    friend H AbslHashValue(H h, const boost::optional<TenantId>& tenantId) {
        if (tenantId)
            h = H::combine(std::move(h), tenantId->_oid);
        return H::combine(std::move(h), tenantId.has_value());
    }

    /**
     * Parse tenant id from BSON. The function is used by IDL parsers.
     */
    static TenantId parseFromBSON(const BSONElement& elem);

    /**
     * Serialize tenant id to BSON. These functions are used by IDL parsers.
     */
    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const;
    void serializeToBSON(BSONArrayBuilder* builder) const;

    friend void appendToBson(BSONObjBuilder& bob, StringData fieldName, const TenantId& value) {
        value.serializeToBSON(fieldName, &bob);
    }

private:
    friend class NamespaceString;
    friend class DatabaseName;

    MONGO_MOD_NEEDS_REPLACEMENT OID _oid;
};

inline bool operator==(const TenantId& lhs, const TenantId& rhs) {
    return lhs.compare(rhs) == 0;
}

inline bool operator!=(const TenantId& lhs, const TenantId& rhs) {
    return !(lhs == rhs);
}

inline bool operator<(const TenantId& lhs, const TenantId& rhs) {
    return lhs.compare(rhs) < 0;
}

inline bool operator>(const TenantId& lhs, const TenantId& rhs) {
    return rhs < lhs;
}

inline bool operator<=(const TenantId& lhs, const TenantId& rhs) {
    return !(lhs > rhs);
}

inline bool operator>=(const TenantId& lhs, const TenantId& rhs) {
    return !(lhs < rhs);
}

inline std::ostream& operator<<(std::ostream& os, const TenantId& tenantId) {
    return os << tenantId.toString();
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& stream,
                                         const TenantId& tenantId) {
    return stream << tenantId.toString();
}

}  // namespace mongo
