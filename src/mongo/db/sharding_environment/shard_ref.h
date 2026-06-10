/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <variant>

namespace mongo {

/**
 * Identifies a shard uniquely. May be either a legacy string name (for backward compatibility with
 * older documents that only store the shard name) or a UUID assigned at shard registration time.
 */
class MONGO_MOD_PUBLIC ShardRef {
public:
    // Required for IDL-generated code which default-constructs the field before parsing.
    ShardRef() : _ref(ShardId{}) {}
    explicit ShardRef(std::string name) : _ref(ShardId(std::move(name))) {}
    explicit ShardRef(UUID uuid) : _ref(std::move(uuid)) {}

    /**
     * Implicit conversion from ShardId.
     *
     * TODO SERVER-127411: this is a transitional convenience while ShardId still exists. Once all
     * catalog types and APIs have been migrated to ShardRef and ShardId has been removed, drop this
     * constructor.
     */
    // NOLINTNEXTLINE(google-explicit-constructor)
    ShardRef(const ShardId& id) : _ref(id) {}

    bool isString() const {
        return std::holds_alternative<ShardId>(_ref);
    }

    bool isUUID() const {
        return std::holds_alternative<UUID>(_ref);
    }

    const std::string& getString() const {
        return std::get<ShardId>(_ref).toString();
    }

    const UUID& getUUID() const {
        return std::get<UUID>(_ref);
    }

    /**
     * Returns the stored ShardId by reference. Only valid when this ShardRef holds a string (i.e.
     * isString() is true); invariants otherwise.
     *
     * TODO SERVER-127411: this is a transitional convenience while ShardId still exists. Once all
     * catalog types and APIs have been migrated to ShardRef and ShardId has been removed, drop this
     * accessor.
     */
    const ShardId& getShardId() const {
        invariant(isString());
        return std::get<ShardId>(_ref);
    }

    std::string toString() const;

    /**
     * Implicit conversion to ShardId.
     *
     * Returns by value on purpose: lifetime extension does not propagate through a reference
     * returned by a conversion operator, so a reference-returning conversion would dangle for
     * temporaries. Callers that want a reference into this ShardRef should use getShardId().
     *
     * TODO SERVER-127411: this is a transitional convenience while ShardId still exists. Once all
     * catalog types and APIs have been migrated to ShardRef and ShardId has been removed, drop this
     * conversion.
     */
    // NOLINTNEXTLINE(google-explicit-constructor)
    operator ShardId() const;

    bool operator==(const ShardRef& other) const {
        return _ref == other._ref;
    }

    bool operator!=(const ShardRef& other) const {
        return !(*this == other);
    }

    bool operator<(const ShardRef& other) const {
        return _ref < other._ref;
    }

    bool operator>(const ShardRef& other) const {
        return other < *this;
    }

    bool operator<=(const ShardRef& other) const {
        return !(*this > other);
    }

    bool operator>=(const ShardRef& other) const {
        return !(*this < other);
    }

    /**
     * Hash function compatible with absl::Hash for absl::unordered_{map,set}
     */
    template <typename H>
    friend H AbslHashValue(H h, const ShardRef& ref) {
        if (ref.isString()) {
            return H::combine(std::move(h), ref.getString());
        }
        return H::combine(std::move(h), ref.getUUID());
    }

    static ShardRef parse(const BSONElement& element);
    void serialize(StringData fieldName, BSONObjBuilder* builder) const;
    void serialize(BSONArrayBuilder* builder) const;

    friend void appendToBson(BSONObjBuilder& bob, StringData fieldName, const ShardRef& ref) {
        ref.serialize(fieldName, &bob);
    }

    static Status validate(const ShardRef& value) {
        if (value.isString()) {
            return ShardId::validate(value.getString());
        }
        // UUID variant is always valid.
        return Status::OK();
    }

private:
    std::variant<ShardId, UUID> _ref;
};

/**
 * Convenience comparisons between ShardRef and ShardId so that callers which obtained a ShardId
 * from a shard registry or from the sharding state can compare it against a ShardRef stored in
 * config.databases / config.cache.databases without an explicit conversion.
 *
 * A ShardRef that holds a UUID never compares equal to a ShardId (which is always a string);
 * mixed-type comparisons of the same logical shard should not happen once the migration is
 * complete, because by then all such call sites will operate on ShardRef directly.
 *
 * TODO SERVER-127411: remove these overloads once ShardId has been removed and every comparison
 * site has been migrated to ShardRef.
 */
MONGO_MOD_PUBLIC inline bool operator==(const ShardRef& ref, const ShardId& id) {
    return ref.isString() && ref.getString() == id.toString();
}

MONGO_MOD_PUBLIC inline bool operator==(const ShardId& id, const ShardRef& ref) {
    return ref == id;
}

MONGO_MOD_PUBLIC inline bool operator!=(const ShardRef& ref, const ShardId& id) {
    return !(ref == id);
}

MONGO_MOD_PUBLIC inline bool operator!=(const ShardId& id, const ShardRef& ref) {
    return !(id == ref);
}

/**
 * Streaming operators that route through ShardRef::toString(), which renders both the string and
 * UUID variants safely. These must exist so that streaming a ShardRef does not fall back to the
 * implicit ShardId conversion, which invariants when the ref holds a UUID.
 */
inline std::ostream& operator<<(std::ostream& os, const ShardRef& ref) {
    return os << ref.toString();
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& stream,
                                         const ShardRef& ref) {
    return stream << ref.toString();
}

}  // namespace mongo
