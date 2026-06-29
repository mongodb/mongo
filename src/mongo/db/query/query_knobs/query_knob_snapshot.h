/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_change_notifier.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <variant>
#include <vector>

namespace mongo {

/**
 * Identifies where a knob value in a QueryKnobSnapshot came from.
 */
enum class KnobSource : uint8_t {
    kDefault,       // Value read from global IDL default
    kSetParameter,  // Value overridden via setParameter command
    kQuerySettings  // Value overridden via Parameterized Query Settings (PQS)
};

// KnobSource::kDefault must be the zero enumerator so that value-initialized vectors are
// pre-filled with it.
static_assert(static_cast<uint8_t>(KnobSource::kDefault) == 0,
              "KnobSource::kDefault must be zero for value-initialization to yield kDefault");

/**
 * An immutable, value-semantic snapshot of all query knob values for a single query lifetime.
 * Each slot stores a QueryKnobValue and the KnobSource that last wrote it.
 *
 * Enum-typed knobs are stored as int (matching the QueryKnobValue convention);
 * get<EnumT>() casts back to the enum type.
 *
 * Construct via QueryKnobSnapshotBuilder.
 */
class QueryKnobSnapshot {
    friend class QueryKnobSnapshotBuilder;

public:
    QueryKnobSnapshot(const QueryKnobSnapshot&) = default;
    QueryKnobSnapshot& operator=(const QueryKnobSnapshot&) = default;
    QueryKnobSnapshot(QueryKnobSnapshot&&) noexcept = default;
    QueryKnobSnapshot& operator=(QueryKnobSnapshot&&) noexcept = default;

    QueryKnobSnapshot clone() const;

    template <typename T>
    T get(QueryKnobId id) const {
        auto&& val = getValue(id);
        if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(std::get<int>(val));
        } else {
            return std::get<T>(val);
        }
    }

    const QueryKnobValue& getValue(QueryKnobId id) const {
        tassert(12312300, "QueryKnobSnapshot index out of bounds", id.value < size());
        return _values[id.value];
    }

    KnobSource getSource(QueryKnobId id) const;

    size_t size() const;

private:
    QueryKnobSnapshot(std::vector<QueryKnobValue> values, std::vector<KnobSource> sources);

    struct Storage {
        std::vector<QueryKnobValue> values;
        std::vector<KnobSource> sources;
    };
    std::shared_ptr<const Storage> _storage;

    // Cache a span of values for fast access without dereferencing the shared_ptr.
    std::span<const QueryKnobValue> _values = _storage->values;
};

/**
 * Builder for QueryKnobSnapshot. Slots are pre-filled with DeleteQueryKnobOverride /
 * KnobSource::kDefault; call set() for each slot that needs a non-default value, then call build().
 *
 * Supports fluent chaining from a temporary: QueryKnobSnapshotBuilder{n}.set(...).build().
 * For a named builder, move it before building: std::move(builder).build().
 */
class QueryKnobSnapshotBuilder {
public:
    explicit QueryKnobSnapshotBuilder(size_t size);
    explicit QueryKnobSnapshotBuilder(QueryKnobSnapshot snapshot);

    QueryKnobSnapshotBuilder(QueryKnobSnapshotBuilder&&) noexcept = default;
    QueryKnobSnapshotBuilder& operator=(QueryKnobSnapshotBuilder&&) noexcept = default;
    QueryKnobSnapshotBuilder(const QueryKnobSnapshotBuilder&) = delete;
    QueryKnobSnapshotBuilder& operator=(const QueryKnobSnapshotBuilder&) = delete;

    QueryKnobSnapshotBuilder& set(QueryKnobId id, QueryKnobValue value, KnobSource source);

    [[nodiscard]] QueryKnobSnapshot build() &&;

private:
    std::vector<QueryKnobValue> _values;
    std::vector<KnobSource> _sources;
};

/**
 * Thread-safe store for a QueryKnobSnapshot. Reads are optimized via WriteRarelyRWMutex;
 * writes are serialized by _intentLock to prevent lost updates from concurrent callers.
 * The outgoing snapshot is swapped out under the write lock and destroyed after it is released.
 */
class QueryKnobSnapshotCache {
public:
    explicit QueryKnobSnapshotCache(QueryKnobSnapshot defaults);

    static const QueryKnobSnapshotCache& instance();

    /**
     * Returns a copy of the current snapshot. Safe to call concurrently from any thread.
     */
    [[nodiscard]] QueryKnobSnapshot getSnapshot() const;
    [[nodiscard]] QueryKnobSnapshot getDefaults() const;

    /**
     * Returns a copy of the current snapshot, cached in thread-local storage. Safe to call
     * concurrently from any thread.
     */
    [[nodiscard]] QueryKnobSnapshot getThreadLocalSnapshot() const {
        thread_local boost::optional<QueryKnobSnapshot> localCache = boost::none;
        thread_local uint64_t localVersion = 0;
        auto currentVersion = _version.loadRelaxed();
        const bool cacheHit = localCache && localVersion == currentVersion;
        if (MONGO_unlikely(!cacheHit)) {
            localCache = getSnapshot().clone();
            localVersion = currentVersion;
        }
        return *localCache;
    }

    /**
     * Updates a single knob in the snapshot. Serialized against concurrent writers; readers
     * are unaffected except for a brief stall when the new snapshot is installed.
     *
     * When called with a QueryKnobChange, the source (kDefault vs kSetParameter) is inferred
     * by comparing the new value against '_defaults'.
     */
    void updateKnobValue(const QueryKnobChange&);
    void updateKnobValue(QueryKnobId id, QueryKnobValue value, KnobSource source);

private:
    mutable WriteRarelyRWMutex _rwLock;
    std::mutex _intentLock;
    QueryKnobSnapshot _defaults;  // immutable reference for kDefault vs kSetParameter detection
    QueryKnobSnapshot _snapshot;  // live snapshot used by queries
    alignas(64) AtomicWord<uint8_t> _version{0};
};

}  // namespace mongo
