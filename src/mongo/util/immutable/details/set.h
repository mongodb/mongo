/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <algorithm>

namespace mongo::immutable::details::set {

template <typename set, typename SearchKey>
bool equal(const typename set::key_type& a, const SearchKey& b) {
    return !typename set::comp{}(a, b) && !typename set::comp{}(b, a);
}

template <typename set, class SearchKey>
[[nodiscard]] typename set::iterator lower_bound(const typename set::storage_type& storage,
                                                 const SearchKey& key) {
    return std::lower_bound(storage.begin(), storage.end(), key, typename set::comp{});
}

template <typename set, typename SearchKey>
[[nodiscard]] typename set::iterator find(const typename set::storage_type& storage,
                                          const SearchKey& key) {
    auto it = lower_bound<set>(storage, key);
    if (it != storage.end() && equal<set>(*it, key)) {
        return it;
    }

    return storage.end();
}

template <typename set, typename S, typename K>
[[nodiscard]] typename set::storage_type insert(S&& storage, K&& key) {
    auto it = lower_bound<set>(storage, key);
    if (it == storage.end()) {
        return std::forward<S>(storage).push_back(std::forward<K>(key));
    }

    if (equal<set>(*it, key)) {
        return std::forward<S>(storage);
    }

    return std::forward<S>(storage).insert(it.index(), std::forward<K>(key));
}

template <typename set, typename S, typename K>
[[nodiscard]] typename set::storage_type insert(S&& storage, typename set::iterator it, K&& key) {
    if (it != storage.end() && equal<set>(*it, key)) {
        return std::forward<S>(storage);
    }

    if (it == storage.end()) {
        if (typename set::comp{}(storage[storage.size() - 1], key)) {
            return std::forward<S>(storage).push_back(std::forward<K>(key));
        }
        return insert<set>(std::forward<S>(storage), std::forward<K>(key));
    }

    if (typename set::comp{}(key, *it) &&
        (it.index() == 0 || typename set::comp{}(*(it - 1), key))) {
        return std::forward<S>(storage).insert(it.index(), std::forward<K>(key));
    }

    return insert<set>(std::forward<S>(storage), std::forward<K>(key));
}

template <typename set, typename S, typename K>
[[nodiscard]] typename set::storage_type erase(S&& storage, K&& key) {
    auto it = find<set>(storage, key);
    if (it == storage.end()) {
        return std::forward<S>(storage);
    }
    return std::forward<S>(storage).erase(it.index());
}

template <typename set, typename S, typename K>
[[nodiscard]] typename set::storage_type erase(S&& storage, typename set::iterator it, K&& key) {
    if (it == storage.end() || !equal<set>(*it, key)) {
        return erase<set>(std::forward<S>(storage), std::forward<K>(key));
    }
    return std::forward<S>(storage).erase(it.index());
}

}  // namespace mongo::immutable::details::set
