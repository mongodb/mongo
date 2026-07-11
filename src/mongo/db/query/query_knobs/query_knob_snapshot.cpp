// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_snapshot.h"

#include "mongo/db/query/query_knobs/query_knob_change_notifier.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/static_immortal.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangInQueryKnobSnapshotCacheRead);
MONGO_FAIL_POINT_DEFINE(hangInQueryKnobSnapshotCacheUpdate);

namespace {
static StaticImmortal<std::unique_ptr<QueryKnobSnapshotCache>> gQueryKnobSnapshotCache(nullptr);
};

QueryKnobSnapshot::QueryKnobSnapshot(std::vector<QueryKnobValue> values,
                                     std::vector<KnobSource> sources)
    : _storage(std::make_shared<Storage>(std::move(values), std::move(sources))) {}

QueryKnobSnapshot QueryKnobSnapshot::clone() const {
    return {_storage->values, _storage->sources};
}

KnobSource QueryKnobSnapshot::getSource(QueryKnobId id) const {
    tassert(12312301, "QueryKnobSnapshot index out of bounds", id.value < size());
    return _storage->sources[id.value];
}

size_t QueryKnobSnapshot::size() const {
    return _values.size();
}

QueryKnobSnapshotBuilder::QueryKnobSnapshotBuilder(size_t size) : _values(size), _sources(size) {}

QueryKnobSnapshotBuilder::QueryKnobSnapshotBuilder(QueryKnobSnapshot snapshot)
    : _values(std::move(snapshot._storage->values)),
      _sources(std::move(snapshot._storage->sources)) {}

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

const QueryKnobSnapshotCache& QueryKnobSnapshotCache::instance() {
    tassert(12736300, "QueryKnobSnapshotCache is not initialized", *gQueryKnobSnapshotCache);
    return **gQueryKnobSnapshotCache;
}

QueryKnobSnapshotCache::QueryKnobSnapshotCache(QueryKnobSnapshot defaults)
    : _defaults(std::move(defaults)), _snapshot(_defaults) {}

QueryKnobSnapshot QueryKnobSnapshotCache::getSnapshot() const {
    auto readLock = _rwLock.readLock();
    if (MONGO_unlikely(hangInQueryKnobSnapshotCacheRead.shouldFail())) {
        hangInQueryKnobSnapshotCacheRead.pauseWhileSet();
    }
    return _snapshot;
}

QueryKnobSnapshot QueryKnobSnapshotCache::getDefaults() const {
    return _defaults;
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
    _version.fetchAndAdd(1);
}

void QueryKnobSnapshotCache::updateKnobValue(const QueryKnobChange& change) {
    auto&& [id, value] = change;
    const bool isDefault = _defaults.getValue(id) == value;
    updateKnobValue(id, value, isDefault ? KnobSource::kDefault : KnobSource::kSetParameter);
}

REGISTER_QUERY_KNOBS_LISTENER(QueryKnobSnapshotCacheUpdater, [](const QueryKnobChange& change) {
    static auto* cache = [] {
        invariant(*gQueryKnobSnapshotCache);
        return gQueryKnobSnapshotCache->get();
    }();
    cache->updateKnobValue(change);
    return Status::OK();
})

MONGO_INITIALIZER_GENERAL(QueryKnobSnapshotCacheInit,
                          ("QueryKnobRegistryInit"),
                          ("EndQueryKnobChangeListenerRegistration"))(InitializerContext*) {
    auto&& entries = QueryKnobRegistry::instance().entries();
    QueryKnobSnapshotBuilder builder(entries.size());
    for (auto&& entry : entries) {
        builder.set(entry.id, entry.readGlobal(), KnobSource::kDefault);
    }
    *gQueryKnobSnapshotCache = std::make_unique<QueryKnobSnapshotCache>(std::move(builder).build());
};

}  // namespace mongo
