// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/immutable/details/memory_policy.h"
#include "mongo/util/immutable/details/set.h"
#include "mongo/util/modules.h"

#include <algorithm>

#include <immer/flex_vector.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::immutable {

/**
 * Immutable ordered set.
 *
 * Interfaces that "modify" the set are 'const' and return a new version of the set with the
 * modifications applied and leaves the original version untouched.
 *
 * It is optimized for efficient copies and low memory usage when multiple versions of the set exist
 * simultaneously at the expense of regular lookups not being as efficient as the regular std
 * ordered containers. Suitable for use in code that uses the copy-on-write pattern.
 *
 * Thread-safety: All methods are const, it is safe to perform modifications that result in new
 * versions from multiple threads concurrently.
 *
 * Memory management: Internal memory management is done using reference counting, memory is free'd
 * as references to different versions of the set are released.
 *
 * Built on top  of 'immer::flex_vector'.
 * Documentation: 'immer/flex_vector.h' and https://sinusoid.es/immer/
 */
template <typename Key, typename Compare = std::less<Key>>
class set {
public:
    using key_type = Key;
    using storage_type = immer::flex_vector<key_type, detail::MemoryPolicy>;
    using iterator = typename storage_type::iterator;
    using size_type = typename storage_type::size_type;
    using diference_type = std::ptrdiff_t;
    using reference = const key_type&;
    using const_reference = const key_type&;
    using memory_policy_type = detail::MemoryPolicy;
    using comp = Compare;

    set() = default;

    bool operator==(const set& other) const {
        return _storage == other._storage;
    }

    bool operator!=(const set& other) const {
        return !(*this == other);
    }

    [[nodiscard]] iterator begin() const {
        return _storage.begin();
    }

    [[nodiscard]] iterator end() const {
        return _storage.end();
    }

    size_t size() const {
        return _storage.size();
    }

    /**
     * Insert a new 'key' to the set.
     *
     * Returns the modified set, or the original if 'key' was already contained.
     */
    template <typename K>
    [[nodiscard]] set insert(K&& key) const& {
        return set{details::set::insert<set>(_storage, std::forward<K>(key))};
    }
    template <typename K>
    [[nodiscard]] set insert(K&& key) && {
        return set{details::set::insert<set>(std::move(_storage), std::forward<K>(key))};
    }

    /**
     * Insert a new 'key' to the set. Uses 'it' as a hint.
     *
     * Returns the modified set, or the original if 'key' was already contained.
     */
    template <typename K>
    [[nodiscard]] set insert(iterator it, K&& key) const& {
        return set{details::set::insert<set>(_storage, it, std::forward<K>(key))};
    }
    template <typename K>
    [[nodiscard]] set insert(iterator it, K&& key) && {
        return set{details::set::insert<set>(std::move(_storage), it, std::forward<K>(key))};
    }

    /**
     * Removes 'key' from the set.
     *
     * Returns the modified set, or the original if 'key' does not exist.
     */
    template <typename K>
    [[nodiscard]] set erase(K&& key) const& {
        return set{details::set::erase<set>(_storage, std::forward<K>(key))};
    }
    template <typename K>
    [[nodiscard]] set erase(K&& key) && {
        return set{details::set::erase<set>(std::move(_storage), std::forward<K>(key))};
    }

    /**
     * Removes key associated with 'key' from the set. Uses 'it' as a hint.
     *
     * Returns the modified set, or the original if 'key' does not exist.
     */
    template <typename K>
    [[nodiscard]] set erase(iterator it, K&& key) const& {
        return set{details::set::erase<set>(_storage, it, std::forward<K>(key))};
    }
    template <typename K>
    [[nodiscard]] set erase(iterator it, K&& key) && {
        return set{details::set::erase<set>(std::move(_storage), it, std::forward<K>(key))};
    }

    /**
     * Returns an iterator to the element for 'key' if it exists, 'end()' otherwise.
     *
     * Supports heterogeneous lookup.
     */
    template <class SearchKey>
    [[nodiscard]] iterator find(const SearchKey& key) const {
        return details::set::find<set>(_storage, key);
    }

    /**
     * Returns true if set contains 'key'.
     *
     * Supports heterogeneous lookup.
     */
    template <class SearchKey>
    bool contains(const SearchKey& key) const {
        return find(key) != end();
    }

    /**
     * Returns the first the element greater than or equal to 'key' if one exists, 'end()'
     * otherwise.
     *
     * Supports heterogeneous lookup.
     */
    template <class SearchKey>
    [[nodiscard]] iterator lower_bound(const SearchKey& key) const {
        return details::set::lower_bound<set>(_storage, key);
    }

    /**
     * Returns the first the element strictly greater than 'key' if one exists, 'end()' otherwise.
     *
     * Supports heterogeneous lookup.
     */
    template <class SearchKey>
    [[nodiscard]] iterator upper_bound(const SearchKey& key) const {
        return std::upper_bound(_storage.begin(), _storage.end(), key, comp{});
    }

private:
    template <typename S>
    explicit set(S&& s) : _storage{std::forward<S>(s)} {}

    storage_type _storage;
};

}  // namespace mongo::immutable
