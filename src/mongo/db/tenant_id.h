// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/modules.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <new>
#include <ostream>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
     * Parse a tenantId from a std::string_view. This method asserts if the tenantId is empty or not
     * in an OID format. Returns a TenantId object from the parsed string.
     */
    static TenantId parseFromString(std::string_view tenantId);

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
    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const;
    void serializeToBSON(BSONArrayBuilder* builder) const;

    friend void appendToBson(BSONObjBuilder& bob,
                             std::string_view fieldName,
                             const TenantId& value) {
        value.serializeToBSON(fieldName, &bob);
    }

private:
    friend class NamespaceString;
    friend class DatabaseName;

    [[MONGO_MOD_NEEDS_REPLACEMENT]] OID _oid;
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
