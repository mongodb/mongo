// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

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
