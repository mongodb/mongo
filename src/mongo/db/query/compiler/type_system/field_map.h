// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <boost/intrusive_ptr.hpp>

namespace mongo::pipeline::type_system {

/**
 * Maps a key to a value, kept sorted by key.
 * Refcounted, to provide structural sharing of deep hierarchies, so the storage is never mutated
 * unless we are the only owner.
 */
template <typename T>
class FieldMap {
public:
    /// An empty map.
    FieldMap() = default;

    bool empty() const {
        tassert(13459101, "Held storage is never empty", !_storage || !_storage->entries.empty());
        return !_storage;
    }

    size_t size() const {
        return _storage ? _storage->entries.size() : 0;
    }

    auto begin() const {
        return entries().begin();
    }

    auto end() const {
        return entries().end();
    }

    /// Lookup the value for 'key'.
    const T* find(std::string_view key) const {
        if (!_storage) {
            return nullptr;
        }
        auto it = _storage->entries.find(key);
        return it != _storage->entries.end() ? &it->second : nullptr;
    }

    /// Sets the value for 'key'. Does not clone if the held value is the same.
    void set(std::string_view key, T value) {
        if (isShared()) {
            // Do not clone unless the value changes.
            if (const T* held = find(key); held && *held == value) {
                return;
            }
        }
        store(key, std::move(value));
    }

    /// Removes 'key'. No-op if 'key' is absent.
    void erase(std::string_view key) {
        if (!_storage) {
            return;
        }
        if (isShared() && !find(key)) {
            return;
        }
        auto& stored = ensureMutable();
        if (auto it = stored.find(key); it != stored.end()) {
            stored.erase(it);
            resetIfEmpty();
        }
    }

    /// Returns true if the map shares storage with 'other', so the two are equal without comparing.
    bool sameStorage(const FieldMap& other) const {
        return _storage == other._storage;
    }

    /**
     * Updates the value for 'key' by applying 'transform' to it. If there is no existing value,
     * 'implied' is used. Removes 'key' if the new value is 'implied'.
     * Hands the existing value over to 'transform', so that a value owning a nested map can be
     * rewritten without copying it.
     * Only basic exception safety: a throw from 'transform' leaves the map unspecified.
     */
    template <typename Transform>
    void update(std::string_view key, const T& implied, Transform transform) {
        if (_storage && !isShared()) {
            // We own the storage, so the key only has to be looked up once.
            auto& stored = _storage->entries;
            auto it = stored.find(key);
            T updated = it != stored.end() ? transform(std::move(it->second)) : transform(implied);
            if (updated == implied) {
                if (it != stored.end()) {
                    stored.erase(it);
                    resetIfEmpty();
                }
                return;
            }
            if (it != stored.end()) {
                it->second = std::move(updated);
                return;
            }
            stored.emplace(std::string{key}, std::move(updated));
            return;
        }
        // Do not copy unless the value changes.
        const T* held = find(key);
        T updated = transform(held ? *held : implied);
        if (updated == implied) {
            erase(key);
            return;
        }
        set(key, std::move(updated));
    }

    /**
     * Merges 'rhs' into this map.
     * The result holds every key that either map holds. Each value is what 'combine' returns for
     * the two values held for that key. When only one map holds the key, the missing side is
     * passed as 'lhsImplied' or 'rhsImplied'. A key whose combined value is 'mergedImplied' is
     * removed instead of stored.
     * The entries are mutated in place while we are the only owner, so no third storage is made.
     * Only basic exception safety: a throw from 'combine' leaves the map unspecified.
     */
    template <typename Combine>
    void merge(const FieldMap& rhs,
               const T& lhsImplied,
               const T& rhsImplied,
               const T& mergedImplied,
               Combine combine) {
        if (this == &rhs) {
            // We mutate 'lhs' as we read 'rhs' below, so we need to prevent the case where they are
            // the same object, as then 'rhs' would be changed unexpectedly by writes to 'lhs'.
            const auto shared = rhs;
            merge(shared, lhsImplied, rhsImplied, mergedImplied, std::move(combine));
            return;
        }
        // We will merge into ourselves, which we can only do safely if we are uniquely owned.
        auto& lhs = ensureMutable();
        // The storage has to be released even when 'combine' throws partway through.
        // This is to handle the case where the container is left empty, but the next iteration
        // throws, at which point we exist the function with empty container instead of none.
        ScopeGuard released([&] { resetIfEmpty(); });
        const auto lhsEnd = lhs.end();
        const auto rhsEnd = rhs.end();
        auto lhsIt = lhs.begin();
        auto rhsIt = rhs.begin();
        // We need to iterate the entries of both maps and:
        // - if both hold the same key, we combine the values
        // - if only lhs or rhs holds the key, we combine the value with the other's implied value
        while (lhsIt != lhsEnd || rhsIt != rhsEnd) {
            // We need to make a three-way decision here:
            // -1 if we need to take from lhs because rhs reached end()
            // 1 if we need to take from rhs because lhs reached end()
            // 0 if we can combine
            const int order =
                lhsIt == lhsEnd ? 1 : (rhsIt == rhsEnd ? -1 : lhsIt->first.compare(rhsIt->first));
            const bool takeLhs = order <= 0;
            const bool takeRhs = order >= 0;

            if (!takeLhs) {
                T merged = combine(lhsImplied, rhsIt->second);
                if (!(merged == mergedImplied)) {
                    lhs.emplace_hint(lhsIt, rhsIt->first, std::move(merged));
                }
                ++rhsIt;
                continue;
            }

            T merged = combine(std::move(lhsIt->second), takeRhs ? rhsIt->second : rhsImplied);
            if (merged == mergedImplied) {
                lhsIt = lhs.erase(lhsIt);
            } else {
                lhsIt->second = std::move(merged);
                ++lhsIt;
            }
            if (takeRhs) {
                ++rhsIt;
            }
        }
    }

    /// Returns true if 'other' holds the same values for the same keys.
    bool operator==(const FieldMap& other) const {
        return sameStorage(other) || entries() == other.entries();
    }

    /// Returns true if the map shares its storage with another instance.
    bool isShared() const {
        return _storage && _storage->isShared();
    }

    /// Returns the address of the storage, which tells a clone apart from a reuse.
    const void* getStorage_forTest() const {
        return _storage.get();
    }

private:
    struct Storage final : RefCountable {
        std::map<std::string, T, std::less<>> entries;
    };

    /// Returns the entries, or an empty map when no storage is held.
    const auto& entries() const {
        static const std::map<std::string, T, std::less<>> kNone;
        return _storage ? _storage->entries : kNone;
    }

    /// Releases the storage once nothing is held, so an empty map has exactly one encoding.
    void resetIfEmpty() {
        if (_storage->entries.empty()) {
            _storage.reset();
        }
    }

    /// Returns the entries for writing, using copy-on-write.
    auto& ensureMutable() {
        if (!_storage) {
            _storage = make_intrusive<Storage>();
        } else if (_storage->isShared()) {
            auto cloned = make_intrusive<Storage>();
            cloned->entries = _storage->entries;
            _storage = std::move(cloned);
        }
        return _storage->entries;
    }

    /// Writes 'value' for 'key' into storage we own, looking the key up only once.
    void store(std::string_view key, T value) {
        auto& stored = ensureMutable();
        if (auto it = stored.find(key); it != stored.end()) {
            // Assigning through the entry found above leaves the stored name alone.
            it->second = std::move(value);
            return;
        }
        stored.emplace(std::string{key}, std::move(value));
    }

    /// nullptr when the map is empty, so an empty map has one encoding.
    boost::intrusive_ptr<Storage> _storage;
};

}  // namespace mongo::pipeline::type_system
