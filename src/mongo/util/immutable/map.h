// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/immutable/details/map.h"
#include "mongo/util/immutable/details/memory_policy.h"
#include "mongo/util/modules.h"

#include <algorithm>

#include <immer/flex_vector.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::immutable {

/**
 * Immutable ordered dictionary.
 *
 * Interfaces that "modify" the map are 'const' and return a new version of the map with the
 * modifications applied and leaves the original version untouched.
 *
 * It is optimized for efficient copies and low memory usage when multiple versions of the map exist
 * simultaneously at the expense of regular lookups not being as efficient as the regular std
 * ordered containers. Suitable for use in code that uses the copy-on-write pattern.
 *
 * Thread-safety: All methods are const, it is safe to perform modifications that result in new
 * versions from multiple threads concurrently.
 *
 * Memory management: Internal memory management is done using reference counting, memory is free'd
 * as references to different versions of the map are released.
 *
 * Built on top  of 'immer::flex_vector'.
 * Documentation: 'immer/flex_vector.h' and https://sinusoid.es/immer/
 */
template <typename Key, typename Value, typename Compare = std::less<Key>>
class map {
public:
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<Key, Value>;
    using storage_type = immer::flex_vector<value_type, detail::MemoryPolicy>;
    using iterator = typename storage_type::iterator;
    using size_type = typename storage_type::size_type;
    using diference_type = std::ptrdiff_t;
    using reference = const value_type&;
    using const_reference = const value_type&;
    using memory_policy_type = detail::MemoryPolicy;
    using comp = Compare;

    map() = default;

    struct default_value {
        const mapped_type& operator()() const {
            static mapped_type v = mapped_type{};
            return v;
        }
    };

    struct error_value {
        const mapped_type& operator()() const {
            throw std::out_of_range{"key not found"};
        }
    };

    bool operator==(const map& other) const {
        return _storage == other._storage;
    }

    bool operator!=(const map& other) const {
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
     * Returns a reference to the value associated with 'key' if one exists, otherwise a default
     * constructed Value.
     */
    const mapped_type& operator[](const key_type& key) const {
        auto it = find(key);
        if (it == end()) {
            return default_value{}();
        }
        return it->second;
    }

    /**
     * Returns a reference to the value associated with 'key' if one exists, otherwise throws.
     */
    const mapped_type& at(const key_type& key) const {
        auto it = find(key);
        if (it == end()) {
            return error_value{}();
        }
        return it->second;
    }

    /**
     * Insert a new 'key' and 'value' pair to the map.
     *
     * Returns the modified map, or the original if 'key' was already contained.
     */
    template <typename K, typename V>
    [[nodiscard]] map insert(K&& key, V&& value) const& {
        return map{details::map::insert<map<key_type, mapped_type, comp>>(
            _storage, std::forward<K>(key), std::forward<V>(value))};
    }
    template <typename K, typename V>
    [[nodiscard]] map insert(K&& key, V&& value) && {
        return map{details::map::insert<map<key_type, mapped_type, comp>>(
            std::move(_storage), std::forward<K>(key), std::forward<V>(value))};
    }

    /**
     * Insert a new 'key' and 'value' pair to the map. Uses 'it' as a hint.
     *
     * Returns the modified map, or the original if 'key' was already contained.
     */
    template <typename K, typename V>
    [[nodiscard]] map insert(iterator it, K&& key, V&& value) const& {
        return map{details::map::insert<map<key_type, mapped_type, comp>>(
            _storage, it, std::forward<K>(key), std::forward<V>(value))};
    }
    template <typename K, typename V>
    [[nodiscard]] map insert(iterator it, K&& key, V&& value) && {
        return map{details::map::insert<map<key_type, mapped_type, comp>>(
            std::move(_storage), it, std::forward<K>(key), std::forward<V>(value))};
    }

    /**
     * Sets the value associated with 'key' to 'value', inserting the new pair if 'key' does not
     * exist.
     *
     * Returns the modified map.
     */
    template <typename K, typename V>
    [[nodiscard]] map set(K&& key, V&& value) const& {
        return map{details::map::set<map<key_type, mapped_type, comp>>(
            _storage, std::forward<K>(key), std::forward<V>(value))};
    }
    template <typename K, typename V>
    [[nodiscard]] map set(K&& key, V&& value) && {
        return map{details::map::set<map<key_type, mapped_type, comp>>(
            std::move(_storage), std::forward<K>(key), std::forward<V>(value))};
    }

    /**
     * Sets the value associated with 'key' to 'value', inserting the new pair if 'key' does not
     * exist. Treats 'it' as a hint.
     *
     * Returns the modified map.
     */
    template <typename K, typename V>
    [[nodiscard]] map set(iterator it, K&& key, V&& value) const& {
        return map{details::map::set<map<key_type, mapped_type, comp>>(
            _storage, it, std::forward<K>(key), std::forward<V>(value))};
    }
    template <typename K, typename V>
    [[nodiscard]] map set(iterator it, K&& key, V&& value) && {
        return map{details::map::set<map<key_type, mapped_type, comp>>(
            std::move(_storage), it, std::forward<K>(key), std::forward<V>(value))};
    }

    /**
     * Sets the value associated with 'key' by applying 'valueUpdate' to the existing value, or to a
     * default-constructed value if no entry for 'key' exists.
     *
     * The signature of 'valueUpdate' should be equivalent to
     * std::function<mapped_type(const mapped_type&)>. Returns the modified map.
     */
    template <typename K, typename U>
    [[nodiscard]] map update(K&& key, U&& valueUpdate) const& {
        return map{details::map::update<map<key_type, mapped_type, comp>>(
            _storage, std::forward<K>(key), std::forward<U>(valueUpdate))};
    }
    template <typename K, typename U>
    [[nodiscard]] map update(K&& key, U&& valueUpdate) && {
        return map{details::map::update<map<key_type, mapped_type, comp>>(
            std::move(_storage), std::forward<K>(key), std::forward<U>(valueUpdate))};
    }

    /**
     * Updates the value associated with 'key' by applying 'valueUpdate' to the existing value, or
     * to a default-constructed value if no entry for 'key' exists. Uses 'it' as a hint.
     *
     * The signature of 'valueUpdate' should be equivalent to
     * std::function<mapped_type(const mapped_type&)>. Returns the modified map.
     */
    template <typename K, typename U>
    [[nodiscard]] map update(iterator it, K&& key, U&& valueUpdate) const& {
        return map{details::map::update<map<key_type, mapped_type, comp>>(
            _storage, it, std::forward<K>(key), std::forward<U>(valueUpdate))};
    }
    template <typename K, typename U>
    [[nodiscard]] map update(iterator it, K&& key, U&& valueUpdate) && {
        return map{details::map::update<map<key_type, mapped_type, comp>>(
            std::move(_storage), it, std::forward<K>(key), std::forward<U>(valueUpdate))};
    }

    /**
     * Updates the value associated with 'key' if it exists by applying 'valueUpdate' to the
     * existing value.
     *
     * The signature of 'valueUpdate' should be equivalent to
     * std::function<mapped_type(const mapped_type&)>. Returns the modified map, or the original if
     * 'key' does not exist.
     */
    template <typename K, typename U>
    [[nodiscard]] map update_if_exists(K&& key, U&& valueUpdate) const& {
        return map{details::map::update_if_exists<map<key_type, mapped_type, comp>>(
            _storage, std::forward<K>(key), std::forward<U>(valueUpdate))};
    }
    template <typename K, typename U>
    [[nodiscard]] map update_if_exists(K&& key, U&& valueUpdate) && {
        return map{details::map::update_if_exists<map<key_type, mapped_type, comp>>(
            std::move(_storage), std::forward<K>(key), std::forward<U>(valueUpdate))};
    }

    /**
     * Updates the value associated with 'key' if it exists by applying 'valueUpdate' to the
     * existing value. Uses 'it' as a hint.
     *
     * The signature of 'valueUpdate' should be equivalent to
     * std::function<mapped_type(const mapped_type&)>. Returns the modified map, or the original if
     * 'key' does not exist.
     */
    template <typename K, typename U>
    [[nodiscard]] map update_if_exists(iterator it, K&& key, U&& valueUpdate) const& {
        return map{details::map::update_if_exists<map<key_type, mapped_type, comp>>(
            _storage, it, std::forward<K>(key), std::forward<U>(valueUpdate))};
    }
    template <typename K, typename U>
    [[nodiscard]] map update_if_exists(iterator it, K&& key, U&& valueUpdate) && {
        return map{details::map::update_if_exists<map<key_type, mapped_type, comp>>(
            std::move(_storage), it, std::forward<K>(key), std::forward<U>(valueUpdate))};
    }

    /**
     * Removes 'key' and its associated value from the map.
     *
     * Returns the modified map, or the original if 'key' does not exist.
     */
    template <typename K>
    [[nodiscard]] map erase(K&& key) const& {
        return map{
            details::map::erase<map<key_type, mapped_type, comp>>(_storage, std::forward<K>(key))};
    }
    template <typename K>
    [[nodiscard]] map erase(K&& key) && {
        return map{details::map::erase<map<key_type, mapped_type, comp>>(std::move(_storage),
                                                                         std::forward<K>(key))};
    }

    /**
     * Removes entry assocated with 'it' from the map. 'it' must match 'key' or it will not be
     * erased.
     *
     * Returns the modified map, or the original if 'it' is equal to 'end()'.
     */
    template <typename K>
    [[nodiscard]] map erase(iterator it, K&& key) const& {
        return map{details::map::erase<map<key_type, mapped_type, comp>>(
            _storage, it, std::forward<K>(key))};
    }
    template <typename K>
    [[nodiscard]] map erase(iterator it, K&& key) && {
        return map{details::map::erase<map<key_type, mapped_type, comp>>(
            std::move(_storage), it, std::forward<K>(key))};
    }

    /**
     * Returns an iterator to the entry for 'key' if it exists, 'end()' otherwise.
     *
     * Supports heterogeneous lookup.
     */
    template <typename SearchKey>
    [[nodiscard]] iterator find(const SearchKey& key) const {
        return details::map::find<map<key_type, mapped_type, comp>>(_storage, key);
    }

    /**
     * Returns true if map contains an entry for 'key'.
     *
     * Supports heterogeneous lookup.
     */
    template <typename SearchKey>
    bool contains(const SearchKey& key) const {
        return find(key) != end();
    }

    /**
     * Returns the first the entry greater than or equal to 'key' if one exists, 'end()' otherwise.
     *
     * Supports heterogeneous lookup.
     */
    template <typename SearchKey>
    [[nodiscard]] iterator lower_bound(const SearchKey& key) const {
        return details::map::lower_bound<map<key_type, mapped_type, comp>>(_storage, key);
    }

    /**
     * Returns the first the entry strictly greater than 'key' if one exists, 'end()' otherwise.
     *
     * Supports heterogeneous lookup.
     */
    template <typename SearchKey>
    [[nodiscard]] iterator upper_bound(const SearchKey& key) const {
        return std::upper_bound(
            _storage.begin(),
            _storage.end(),
            key,
            [](const SearchKey& a, const value_type& b) -> bool { return comp{}(a, b.first); });
    }

private:
    template <typename S>
    explicit map(S&& s) : _storage{std::forward<S>(s)} {}

    storage_type _storage;
};

}  // namespace mongo::immutable
