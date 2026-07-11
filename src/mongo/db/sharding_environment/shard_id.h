// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/modules.h"
#include "mongo/util/pcre.h"

#include <cstring>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 *  Representation of a shard identifier.
 */
class ShardId {
public:
    static const ShardId kConfigServerId;

    ShardId(std::string shardId) : _shardId(std::move(shardId)) {}

    ShardId() = default;

    operator std::string_view() const {
        return std::string_view(_shardId);
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
    static Status validate(const ShardId& value);

    /**
     *  Returns true if _shardId is a shard url.
     */
    bool isShardURL() const;

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

    /**
     * Hash function compatible with absl::Hash for absl::unordered_{map,set}
     */
    template <typename H>
    friend H AbslHashValue(H h, const ShardId& shardId) {
        return H::combine(std::move(h), shardId.toString());
    }

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
