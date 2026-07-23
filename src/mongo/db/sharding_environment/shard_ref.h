// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
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

    bool operator==(const ShardRef& other) const {
        return _ref == other._ref;
    }

    bool operator!=(const ShardRef& other) const {
        return !(*this == other);
    }

    static ShardRef parse(const BSONElement& element);
    void serialize(std::string_view fieldName, BSONObjBuilder* builder) const;

private:
    std::variant<std::string, UUID> _ref;
};
}  // namespace mongo
