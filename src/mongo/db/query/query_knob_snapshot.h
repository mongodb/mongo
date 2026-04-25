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

#include "mongo/db/query/query_knob.h"
#include "mongo/util/assert_util.h"

#include <type_traits>
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

    template <typename T>
    T get(size_t index) const {
        tassert(12312300, "QueryKnobSnapshot index out of bounds", index < _values.size());
        const QueryKnobValue& val = _values[index];
        if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(std::get<int>(val));
        } else {
            return std::get<T>(val);
        }
    }

    KnobSource getSource(size_t index) const {
        tassert(12312301, "QueryKnobSnapshot index out of bounds", index < _sources.size());
        return _sources[index];
    }

    size_t size() const {
        return _values.size();
    }

private:
    QueryKnobSnapshot(std::vector<QueryKnobValue> values, std::vector<KnobSource> sources)
        : _values(std::move(values)), _sources(std::move(sources)) {}

    std::vector<QueryKnobValue> _values;
    std::vector<KnobSource> _sources;
};

/**
 * Builder for QueryKnobSnapshot. Slots are pre-filled with monostate / KnobSource::kDefault;
 * call set() for each slot that needs a non-default value, then call build().
 *
 * Supports fluent chaining from a temporary: QueryKnobSnapshotBuilder{n}.set(...).build().
 * For a named builder, move it before building: std::move(builder).build().
 */
class QueryKnobSnapshotBuilder {
public:
    explicit QueryKnobSnapshotBuilder(size_t size) : _values(size), _sources(size) {}

    QueryKnobSnapshotBuilder(QueryKnobSnapshotBuilder&&) noexcept = default;
    QueryKnobSnapshotBuilder& operator=(QueryKnobSnapshotBuilder&&) noexcept = default;
    QueryKnobSnapshotBuilder(const QueryKnobSnapshotBuilder&) = delete;
    QueryKnobSnapshotBuilder& operator=(const QueryKnobSnapshotBuilder&) = delete;

    QueryKnobSnapshotBuilder& set(size_t index, QueryKnobValue value, KnobSource source) {
        tassert(12312302, "QueryKnobSnapshotBuilder index out of bounds", index < _values.size());
        tassert(12312303,
                "QueryKnobSnapshotBuilder::set() value must not be monostate",
                !std::holds_alternative<std::monostate>(value));
        _values[index] = std::move(value);
        _sources[index] = source;
        return *this;
    }

    [[nodiscard]] QueryKnobSnapshot build() && {
        return QueryKnobSnapshot(std::move(_values), std::move(_sources));
    }

private:
    std::vector<QueryKnobValue> _values;
    std::vector<KnobSource> _sources;
};

}  // namespace mongo
