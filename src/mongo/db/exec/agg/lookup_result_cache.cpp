// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/lookup_result_cache.h"

#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec::agg {

boost::optional<Value> LookupResultCache::computeKey(
    const Document& input, const boost::optional<FieldPath>& localFieldPath) {
    if (!isEnabled()) {
        return boost::none;
    }

    Value singleValue;
    int valueCount = 0;
    document_path_support::visitAllValuesAtPath(
        input, *localFieldPath, [&](const Value& nextValue) {
            singleValue = nextValue;
            ++valueCount;
        });

    if (valueCount > 1) {
        return boost::none;
    }
    if (valueCount == 0) {
        return Value(BSONNULL);
    }
    if (singleValue.getType() == BSONType::array || singleValue.getType() == BSONType::object) {
        return boost::none;
    }
    return singleValue;
}

const Value* LookupResultCache::get(const Value& key) {
    auto it = _map.find(key);
    if (it == _map.end()) {
        ++_misses;
        ++_consecutiveMisses;
        maybeFastBailout();
        maybeDisable();
        return nullptr;
    }
    ++_hits;
    _consecutiveMisses = 0;
    _entries.splice(_entries.begin(), _entries, it->second);
    return &it->second->results;
}

void LookupResultCache::put(const Value& key, const Value& results, size_t approxResultsSize) {
    const size_t entrySize = approxResultsSize + key.getApproximateSize();
    if (entrySize > _maxEntrySizeBytes || _maxEntries == 0 || !isEnabled()) {
        return;
    }

    // put() is only ever called after get() has already reported a miss for 'key', so it
    // should never already be present in the map.
    dassert(_map.find(key) == _map.end());
    dassert(_memoryTracker);

    while (_entries.size() >= _maxEntries && !_entries.empty()) {
        evictLru();
    }

    _entries.push_front(Entry{key, results, SimpleMemoryUsageToken(entrySize, _memoryTracker)});
    _map[key] = _entries.begin();
    _totalSizeBytes += entrySize;
}

void LookupResultCache::evictLru() {
    const auto& lru = _entries.back();
    _totalSizeBytes -= static_cast<size_t>(lru.memoryToken.getCurrentMemoryUsageBytes());
    _map.erase(lru.key);
    _entries.pop_back();
}

void LookupResultCache::maybeDisable() {
    if (_state != State::kWarmup || _warmupSize == 0) {
        return;
    }
    const size_t attempts = _hits + _misses;
    if (attempts < _warmupSize) {
        return;
    }
    const size_t hitPercent = (_hits * 100) / attempts;
    if (hitPercent < _minHitPercent) {
        LOGV2_DEBUG(10869601,
                    1,
                    "Disabling $lookup result cache after warmup: hit rate below floor",
                    "hitPercent"_attr = hitPercent,
                    "minHitPercent"_attr = _minHitPercent,
                    "attempts"_attr = attempts);
        disable();
    } else {
        _state = State::kServing;
    }
}

void LookupResultCache::maybeFastBailout() {
    if (_state != State::kWarmup || _fastBailoutMisses == 0) {
        return;
    }
    if (_consecutiveMisses >= _fastBailoutMisses) {
        LOGV2_DEBUG(10869602,
                    1,
                    "Disabling $lookup result cache: fast bailout consecutive-miss threshold "
                    "reached",
                    "consecutiveMisses"_attr = _consecutiveMisses,
                    "fastBailoutMisses"_attr = _fastBailoutMisses);
        disable();
    }
}

void LookupResultCache::disable() {
    _state = State::kDisabled;
    _entries.clear();
    _map.clear();
    _totalSizeBytes = 0;
}

}  // namespace mongo::exec::agg

#undef MONGO_LOGV2_DEFAULT_COMPONENT
