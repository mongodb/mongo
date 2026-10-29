// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"
#include "mongo/util/tracking/string.h"

#include <scoped_allocator>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

struct StringMapHashedKey {
public:
    StringMapHashedKey(Context& Context, std::string_view sd, size_t hash)
        : _Context(Context), _sd(sd), _hash(hash) {}

    operator string() const {
        return make_string(_Context, _sd.data(), _sd.size());
    }

    std::string_view key() const {
        return _sd;
    }

    size_t hash() const {
        return _hash;
    }

private:
    std::reference_wrapper<Context> _Context;
    std::string_view _sd;
    size_t _hash;
};

struct StringMapHasher {
    using is_transparent = void;

    size_t operator()(std::string_view sd) const {
        return absl::Hash<absl::string_view>{}(absl::string_view{sd.data(), sd.size()});
    }

    size_t operator()(const string& s) const {
        return operator()(std::string_view{s.data(), s.size()});
    }

    size_t operator()(StringMapHashedKey key) const {
        return key.hash();
    }

    StringMapHashedKey hashed_key(Context& Context, std::string_view sd) {
        return {Context, sd, operator()(sd)};
    }
};

struct StringMapEq {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const {
        return lhs == rhs;
    }

    bool operator()(const string& lhs, std::string_view rhs) const {
        return std::string_view{lhs.data(), lhs.size()} == rhs;
    }

    bool operator()(std::string_view lhs, const string& rhs) const {
        return lhs == std::string_view{rhs.data(), rhs.size()};
    }

    bool operator()(StringMapHashedKey lhs, StringMapHashedKey rhs) const {
        return lhs.key() == rhs.key();
    }

    bool operator()(const string& lhs, StringMapHashedKey rhs) const {
        return std::string_view{lhs.data(), lhs.size()} == rhs.key();
    }

    bool operator()(StringMapHashedKey lhs, const string& rhs) const {
        return lhs.key() == std::string_view{rhs.data(), rhs.size()};
    }

    bool operator()(const string& lhs, const string& rhs) const {
        return lhs == rhs;
    }
};

template <class Value>
using StringMap =
    absl::flat_hash_map<string,
                        Value,
                        StringMapHasher,
                        StringMapEq,
                        std::scoped_allocator_adaptor<Allocator<std::pair<const string, Value>>>>;

template <class Value>
StringMap<Value> makeStringMap(Context& Context) {
    return StringMap<Value>(
        Context.makeAllocator<typename StringMap<Value>::allocator_type::value_type>());
}

using StringSet = absl::flat_hash_set<string,
                                      StringMapHasher,
                                      StringMapEq,
                                      std::scoped_allocator_adaptor<Allocator<string>>>;

inline StringSet makeStringSet(Context& Context) {
    return StringSet(Context.makeAllocator<typename StringSet::allocator_type::value_type>());
}

}  // namespace tracking
}  // namespace mongo
