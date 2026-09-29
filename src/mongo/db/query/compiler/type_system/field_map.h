// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

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
