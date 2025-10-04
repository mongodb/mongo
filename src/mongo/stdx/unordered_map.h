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

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <absl/container/node_hash_map.h>

namespace MONGO_MOD_PUB mongo {
namespace stdx {

template <class Key, class Value, class Hasher = DefaultHasher<Key>, typename... Args>
using unordered_map = absl::node_hash_map<Key, Value, EnsureTrustedHasher<Hasher, Key>, Args...>;

/**
 * Removes the elements from `c` for which `pred(elem)` is true.
 * Returns the count of elements erased.
 * See https://en.cppreference.com/w/cpp/container/unordered_map/erase_if
 * Workaround for the `void erase(iterator)` of `absl::node_hash_map`.
 */
template <typename Key, typename T, typename Hash, typename Eq, typename Alloc, typename Pred>
size_t erase_if(absl::node_hash_map<Key, T, Hash, Eq, Alloc>& c, Pred&& pred) {
    auto oldSize = c.size();
    for (auto i = c.begin(), last = c.end(); i != last;)
        if (pred(*i))
            c.erase(i++);
        else
            ++i;
    return oldSize - c.size();
}

}  // namespace stdx
}  // namespace MONGO_MOD_PUB mongo
