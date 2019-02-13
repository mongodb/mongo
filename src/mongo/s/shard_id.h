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

#include <ostream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"

namespace mongo {

/**
 *  Representation of a shard identifier.
 */
class ShardId {
public:
    ShardId() = default;
    ShardId(std::string shardId) : _shardId(std::move(shardId)) {}

    operator StringData() const {
        return StringData(_shardId);
    }

    template <size_t N>
    bool operator==(const char (&val)[N]) const {
        return (strncmp(val, _shardId.data(), N) == 0);
    }

    template <size_t N>
    bool operator!=(const char (&val)[N]) const {
        return (strncmp(val, _shardId.data(), N) != 0);
    }

    const std::string& toString() const {
        return _shardId;
    }

    /**
     *  Returns true if _shardId is not empty. Subject to include more validations in the future.
     */
    bool isValid() const;

    /**
     * Returns -1, 0, or 1 if 'this' is less, equal, or greater than 'other' in
     * lexicographical order.
     */
    int compare(const ShardId& other) const;

    /**
     * Functor compatible with std::hash for std::unordered_{map,set}
     */
    struct Hasher {
        std::size_t operator()(const ShardId&) const;
    };

private:
    std::string _shardId;
};

inline bool operator==(const ShardId& lhs, const ShardId& rhs) {
    return lhs.compare(rhs) == 0;
}

inline bool operator!=(const ShardId& lhs, const ShardId& rhs) {
    return !(lhs == rhs);
}

inline bool operator<(const ShardId& lhs, const ShardId& rhs) {
    return lhs.compare(rhs) < 0;
}

inline std::ostream& operator<<(std::ostream& os, const ShardId& shardId) {
    return os << shardId.toString();
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& stream,
                                         const ShardId& shardId) {
    return stream << shardId.toString();
}

}  // namespace mongo
