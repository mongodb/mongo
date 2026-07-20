// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>
#include <variant>

namespace mongo {

/**
 * Identifies a shard uniquely. May be either a legacy string name (for backward compatibility with
 * older documents that only store the shard name) or a UUID assigned at shard registration time.
 */
class [[MONGO_MOD_PUBLIC]] ShardRef {
public:
    // Required for IDL-generated code which default-constructs the field before parsing.
    ShardRef() : _ref(std::string{}) {}
    explicit ShardRef(std::string name) : _ref(std::move(name)) {}
    explicit ShardRef(UUID uuid) : _ref(std::move(uuid)) {}

    /**
     * Implicit conversion from ShardId.
     *
     * TODO SERVER-127411: this is a transitional convenience while ShardId still exists. Once all
     * catalog types and APIs have been migrated to ShardRef and ShardId has been removed, drop this
     * constructor.
     */
    // NOLINTNEXTLINE(google-explicit-constructor)
    ShardRef(const ShardId& id) : _ref(std::string{id.toString()}) {}

    bool isString() const {
        return std::holds_alternative<std::string>(_ref);
    }

    bool isUUID() const {
        return std::holds_alternative<UUID>(_ref);
    }

    const std::string& getString() const {
        return std::get<std::string>(_ref);
    }

    const UUID& getUUID() const {
        return std::get<UUID>(_ref);
    }

    std::string toString() const;

    /**
     * Implicit conversion to ShardId.
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

    static ShardRef parse(const BSONElement& element);
    void serialize(std::string_view fieldName, BSONObjBuilder* builder) const;

    friend void appendToBson(BSONObjBuilder& bob, std::string_view fieldName, const ShardRef& ref) {
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
    std::variant<std::string, UUID> _ref;
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
[[MONGO_MOD_PUBLIC]] inline bool operator==(const ShardRef& ref, const ShardId& id) {
    return ref.isString() && ref.getString() == id.toString();
}

[[MONGO_MOD_PUBLIC]] inline bool operator==(const ShardId& id, const ShardRef& ref) {
    return ref == id;
}

[[MONGO_MOD_PUBLIC]] inline bool operator!=(const ShardRef& ref, const ShardId& id) {
    return !(ref == id);
}

[[MONGO_MOD_PUBLIC]] inline bool operator!=(const ShardId& id, const ShardRef& ref) {
    return !(id == ref);
}

}  // namespace mongo
