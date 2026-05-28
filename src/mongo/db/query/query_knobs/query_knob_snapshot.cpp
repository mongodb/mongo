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

#include "mongo/db/query/query_knobs/query_knob_snapshot.h"

#include "mongo/util/fail_point.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangInQueryKnobSnapshotCacheRead);
MONGO_FAIL_POINT_DEFINE(hangInQueryKnobSnapshotCacheUpdate);

QueryKnobSnapshot::QueryKnobSnapshot(std::vector<QueryKnobValue> values,
                                     std::vector<KnobSource> sources)
    : _values(std::move(values)), _sources(std::move(sources)) {}

KnobSource QueryKnobSnapshot::getSource(QueryKnobId id) const {
    tassert(12312301, "QueryKnobSnapshot index out of bounds", id.value < _sources.size());
    return _sources[id.value];
}

size_t QueryKnobSnapshot::size() const {
    return _values.size();
}

QueryKnobSnapshotBuilder::QueryKnobSnapshotBuilder(size_t size) : _values(size), _sources(size) {}

QueryKnobSnapshotBuilder::QueryKnobSnapshotBuilder(QueryKnobSnapshot snapshot)
    : _values(std::move(snapshot._values)), _sources(std::move(snapshot._sources)) {}

QueryKnobSnapshotBuilder& QueryKnobSnapshotBuilder::set(QueryKnobId id,
                                                        QueryKnobValue value,
                                                        KnobSource source) {
    tassert(12312302, "QueryKnobSnapshotBuilder index out of bounds", id.value < _values.size());
    tassert(12312303,
            "QueryKnobSnapshotBuilder::set() value must not be DeleteQueryKnobOverride",
            !std::holds_alternative<DeleteQueryKnobOverride>(value));
    _values[id.value] = std::move(value);
    _sources[id.value] = source;
    return *this;
}

QueryKnobSnapshot QueryKnobSnapshotBuilder::build() && {
    tassert(12611000,
            "invalid call to QueryKnobSnapshot::build() with unset query knob values",
            std::all_of(_values.cbegin(), _values.cend(), [](const auto& v) -> bool {
                return !std::holds_alternative<DeleteQueryKnobOverride>(v);
            }));
    return QueryKnobSnapshot(std::move(_values), std::move(_sources));
}

QueryKnobSnapshotCache::QueryKnobSnapshotCache(QueryKnobSnapshot snapshot)
    : _snapshot(std::move(snapshot)) {}

QueryKnobSnapshot QueryKnobSnapshotCache::getSnapshot() const {
    auto readLock = _rwLock.readLock();
    if (MONGO_unlikely(hangInQueryKnobSnapshotCacheRead.shouldFail())) {
        hangInQueryKnobSnapshotCacheRead.pauseWhileSet();
    }
    return _snapshot;
}

void QueryKnobSnapshotCache::updateKnobValue(QueryKnobId id,
                                             QueryKnobValue value,
                                             KnobSource source) {
    boost::optional<QueryKnobSnapshot> outgoing;

    // Acquire the intent lock to ensure that only one thread can update the snapshot at a time.
    std::lock_guard lock(_intentLock);

    // Read the current snapshot and layer the new knob value on top.
    QueryKnobSnapshotBuilder builder(_snapshot);
    builder.set(id, value, source);
    outgoing = std::move(builder).build();

    if (MONGO_unlikely(hangInQueryKnobSnapshotCacheUpdate.shouldFail())) {
        hangInQueryKnobSnapshotCacheUpdate.pauseWhileSet();
    }

    // Acquire the write lock and install the new snapshot.
    // Use std::swap() so the outgoing snapshot is destroyed after both locks are released.
    auto writeLock = _rwLock.writeLock();
    std::swap(_snapshot, *outgoing);
}

}  // namespace mongo
