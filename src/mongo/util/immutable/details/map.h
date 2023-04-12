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

namespace mongo::immutable::details::map {

template <typename map, typename SearchKey>
bool equal(const typename map::key_type& a, const SearchKey& b) {
    return !typename map::comp{}(a, b) && !typename map::comp{}(b, a);
}

template <typename map, typename SearchKey>
[[nodiscard]] typename map::iterator lower_bound(const typename map::storage_type& storage,
                                                 const SearchKey& key) {
    return std::lower_bound(storage.begin(),
                            storage.end(),
                            key,
                            [](const typename map::value_type& a, const SearchKey& b) -> bool {
                                return typename map::comp{}(a.first, b);
                            });
}

template <typename map, typename SearchKey>
[[nodiscard]] typename map::iterator find(const typename map::storage_type& storage,
                                          const SearchKey& key) {
    auto it = lower_bound<map>(storage, key);
    if (it != storage.end() && equal<map>(it->first, key)) {
        return it;
    }

    return storage.end();
}

template <typename map, typename S, typename K, typename V>
[[nodiscard]] typename map::storage_type insert(S&& storage, K&& key, V&& value) {
    auto it = lower_bound<map>(storage, key);
    if (it != storage.end() && equal<map>(it->first, key)) {
        return std::forward<S>(storage);
    }
    if (it == storage.end()) {
        return std::forward<S>(storage).push_back(
            std::make_pair(std::forward<K>(key), std::forward<V>(value)));
    }
    return std::forward<S>(storage).insert(
        it.index(), std::make_pair(std::forward<K>(key), std::forward<V>(value)));
}

template <typename map, typename S, typename K, typename V>
[[nodiscard]] typename map::storage_type insert(S&& storage,
                                                typename map::iterator it,
                                                K&& key,
                                                V&& value) {
    if (it != storage.end() && equal<map>(it->first, key)) {
        return std::forward<S>(storage);
    }

    if (it == storage.end()) {
        if (typename map::comp{}(storage[storage.size() - 1].first, key)) {
            return std::forward<S>(storage).push_back(
                std::make_pair(std::forward<K>(key), std::forward<V>(value)));
        }
        return insert<map>(std::forward<S>(storage), std::forward<K>(key), std::forward<V>(value));
    }

    if (typename map::comp{}(key, it->first) &&
        (it.index() == 0 || typename map::comp{}((it - 1)->first, key))) {
        return std::forward<S>(storage).insert(
            it.index(), std::make_pair(std::forward<K>(key), std::forward<V>(value)));
    }

    return insert<map>(std::forward<S>(storage), std::forward<K>(key), std::forward<V>(value));
}

template <typename map, typename S, typename K, typename U>
[[nodiscard]] typename map::storage_type update(S&& storage, K&& key, U&& valueUpdate) {
    auto it = lower_bound<map>(storage, key);
    if (it == storage.end()) {
        return std::forward<S>(storage).push_back(
            std::make_pair(std::forward<K>(key), valueUpdate(typename map::default_value{}())));
    }

    if (equal<map>(it->first, key)) {
        return std::forward<S>(storage).set(
            it.index(), std::make_pair(std::forward<K>(key), valueUpdate(it->second)));
    }

    return insert<map>(std::forward<S>(storage),
                       std::forward<K>(key),
                       valueUpdate(typename map::default_value{}()));
}

template <typename map, typename S, typename K, typename U>
[[nodiscard]] typename map::storage_type update(S&& storage,
                                                typename map::iterator it,
                                                K&& key,
                                                U&& valueUpdate) {
    if (it == storage.end()) {
        if (typename map::comp{}(storage[storage.size() - 1].first, key)) {
            return std::forward<S>(storage).push_back(
                std::make_pair(std::forward<K>(key), valueUpdate(typename map::default_value{}())));
        }
        return update<map>(
            std::forward<S>(storage), std::forward<K>(key), std::forward<U>(valueUpdate));
    }

    if (equal<map>(it->first, key)) {
        return std::forward<S>(storage).set(
            it.index(), std::make_pair(std::forward<K>(key), valueUpdate(it->second)));
    }

    if (typename map::comp{}(key, it->first) &&
        (it.index() == 0 || typename map::comp{}((it - 1)->first, key))) {
        return std::forward<S>(storage).insert(
            it.index(),
            std::make_pair(std::forward<K>(key), valueUpdate(typename map::default_value{}())));
    }

    return insert<map>(std::forward<S>(storage),
                       std::forward<K>(key),
                       valueUpdate(typename map::default_value{}()));
}

template <typename map, typename S, typename K, typename U>
[[nodiscard]] typename map::storage_type update_if_exists(S&& storage, K&& key, U&& valueUpdate) {
    auto it = find<map>(storage, key);
    if (it == storage.end()) {
        return std::forward<S>(storage);
    }
    return std::forward<S>(storage).set(
        it.index(), std::make_pair(std::forward<K>(key), valueUpdate(it->second)));
}

template <typename map, typename S, typename K, typename U>
[[nodiscard]] typename map::storage_type update_if_exists(S&& storage,
                                                          typename map::iterator it,
                                                          K&& key,
                                                          U&& valueUpdate) {
    if (it == storage.end() || !equal<map>(it->first, key)) {
        return update_if_exists<map>(
            std::forward<S>(storage), std::forward<K>(key), std::forward<U>(valueUpdate));
    }
    return std::forward<S>(storage).set(
        it.index(), std::make_pair(std::forward<K>(key), valueUpdate(it->second)));
}

template <typename map, typename S, typename K, typename V>
[[nodiscard]] typename map::storage_type set(S&& storage, K&& key, V&& value) {
    auto it = lower_bound<map>(storage, key);
    if (it == storage.end()) {
        return std::forward<S>(storage).push_back(
            std::make_pair(std::forward<K>(key), std::forward<V>(value)));
    } else if (!equal<map>(it->first, key)) {
        return std::forward<S>(storage).insert(
            it.index(), std::make_pair(std::forward<K>(key), std::forward<V>(value)));
    }
    return std::forward<S>(storage).set(
        it.index(), std::make_pair(std::forward<K>(key), std::forward<V>(value)));
}

template <typename map, typename S, typename K, typename V>
[[nodiscard]] typename map::storage_type set(S&& storage,
                                             typename map::iterator it,
                                             K&& key,
                                             V&& value) {
    if (it == storage.end()) {
        if (typename map::comp{}(storage[storage.size() - 1].first, key)) {
            return std::forward<S>(storage).push_back(
                std::make_pair(std::forward<K>(key), std::forward<V>(value)));
        }
        return set<map>(std::forward<S>(storage), std::forward<K>(key), std::forward<V>(value));
    }

    if (equal<map>(it->first, key)) {
        return std::forward<S>(storage).set(
            it.index(), std::make_pair(std::forward<K>(key), std::forward<V>(value)));
    }

    if (typename map::comp{}(key, it->first) &&
        (it.index() == 0 || typename map::comp{}((it - 1)->first, key))) {
        return std::forward<S>(storage).insert(
            it.index(), std::make_pair(std::forward<K>(key), std::forward<V>(value)));
    }

    return set<map>(std::forward<S>(storage), std::forward<K>(key), std::forward<V>(value));
}

template <typename map, typename S, typename K>
[[nodiscard]] typename map::storage_type erase(S&& storage, K&& key) {
    auto it = find<map>(storage, key);
    if (it == storage.end()) {
        return std::forward<S>(storage);
    }
    return std::forward<S>(storage).erase(it.index());
}

template <typename map, typename S, typename K>
[[nodiscard]] typename map::storage_type erase(S&& storage, typename map::iterator it, K&& key) {
    if (it == storage.end() || !equal<map>(it->first, key)) {
        return erase<map>(std::forward<S>(storage), std::forward<K>(key));
    }
    return std::forward<S>(storage).erase(it.index());
}

}  // namespace mongo::immutable::details::map
