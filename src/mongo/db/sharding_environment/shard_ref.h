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
#include "mongo/util/uuid.h"

#include <string>
#include <variant>

namespace mongo {

/**
 * Identifies a shard uniquely. May be either a legacy string name (for backward compatibility with
 * older documents that only store the shard name) or a UUID assigned at shard registration time.
 */
class ShardRef {
public:
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
    void serialize(StringData fieldName, BSONObjBuilder* builder) const;

private:
    std::variant<std::string, UUID> _ref;
};

}  // namespace mongo
