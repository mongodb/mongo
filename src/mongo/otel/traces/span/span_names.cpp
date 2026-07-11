// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/span/span_names.h"

#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/versioned_value.h"

#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>

namespace mongo::otel::traces {
namespace {
class SpanNameRegistry {
public:
    SpanName& insert(std::string_view name, SampledByDefault sampledByDefault) {
        std::lock_guard lk(_mutex);
        // Commands are constructed once per ClusterRole/Service, so the same command name may be
        // registered more than once.
        auto existing = _spanMap.makeSnapshot();
        if (auto it = existing->find(name); it != existing->end()) {
            invariant(it->second->getSampledByDefault() == sampledByDefault,
                      "duplicate span name registered with a different sampledByDefault");
            return *it->second;
        }
        std::string& storedName = _names.emplace_back(name);
        SpanName& storedSpan = _spans.emplace_back(SpanName(
            SpanName::passkeyForNetworkingAndObservabilityOnly, storedName, sampledByDefault));
        auto newMap = std::make_shared<absl::flat_hash_map<std::string_view, SpanName*>>(*existing);
        invariant(newMap->insert({storedName, &storedSpan}).second);
        _spanMap.update(std::move(newMap));
        return storedSpan;
    }

    const SpanName* lookup(std::string_view name) const {
        thread_local VersionedValue<absl::flat_hash_map<std::string_view, SpanName*>>::Snapshot
            snapshot = _spanMap.makeSnapshot();
        _spanMap.refreshSnapshot(snapshot);
        auto it = snapshot->find(name);
        if (it == snapshot->end()) {
            return nullptr;
        }
        return it->second;
    }

private:
    // Guards writes, as two concurrent writes would clobber each other.
    std::mutex _mutex;
    // Using deque for reference stability.
    std::deque<std::string> _names;
    std::deque<SpanName> _spans;
    // The keys and values here are backed by _names and _spans above.
    VersionedValue<absl::flat_hash_map<std::string_view, SpanName*>> _spanMap{
        std::make_shared<absl::flat_hash_map<std::string_view, SpanName*>>()};
};

SpanNameRegistry& globalSpanNameRegistry() {
    static StaticImmortal<SpanNameRegistry> regInstance;
    return *regInstance;
}
}  // namespace

const SpanName& registerCommandSpanName(std::string_view name, SampledByDefault sampledByDefault) {
    return globalSpanNameRegistry().insert(name, sampledByDefault);
}

const SpanName* lookupCommandSpanName(std::string_view name) {
    return globalSpanNameRegistry().lookup(name);
}

}  // namespace mongo::otel::traces
