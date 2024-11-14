/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <scoped_allocator>

#include "mongo/util/string_map.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"
#include "mongo/util/tracking/string.h"

namespace mongo::tracking {

struct StringMapHashedKey {
public:
    StringMapHashedKey(Context& Context, StringData sd, size_t hash)
        : _Context(Context), _sd(sd), _hash(hash) {}

    operator string() const {
        return make_string(_Context, _sd.data(), _sd.size());
    }

    StringData key() const {
        return _sd;
    }

    size_t hash() const {
        return _hash;
    }

private:
    std::reference_wrapper<Context> _Context;
    StringData _sd;
    size_t _hash;
};

struct StringMapHasher {
    using is_transparent = void;

    size_t operator()(StringData sd) const {
        return absl::Hash<absl::string_view>{}(absl::string_view{sd.data(), sd.size()});
    }

    size_t operator()(const string& s) const {
        return operator()(StringData{s.data(), s.size()});
    }

    size_t operator()(StringMapHashedKey key) const {
        return key.hash();
    }

    StringMapHashedKey hashed_key(Context& Context, StringData sd) {
        return {Context, sd, operator()(sd)};
    }
};

struct StringMapEq {
    using is_transparent = void;

    bool operator()(StringData lhs, StringData rhs) const {
        return lhs == rhs;
    }

    bool operator()(const string& lhs, StringData rhs) const {
        return StringData{lhs.data(), lhs.size()} == rhs;
    }

    bool operator()(StringData lhs, const string& rhs) const {
        return lhs == StringData{rhs.data(), rhs.size()};
    }

    bool operator()(StringMapHashedKey lhs, StringMapHashedKey rhs) const {
        return lhs.key() == rhs.key();
    }

    bool operator()(const string& lhs, StringMapHashedKey rhs) const {
        return StringData{lhs.data(), lhs.size()} == rhs.key();
    }

    bool operator()(StringMapHashedKey lhs, const string& rhs) const {
        return lhs.key() == StringData{rhs.data(), rhs.size()};
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

}  // namespace mongo::tracking
