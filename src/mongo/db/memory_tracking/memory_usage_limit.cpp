// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/memory_usage_limit.h"

#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"

namespace mongo {
namespace {

int64_t readGlobalKnobValue(const QueryKnob<long long>& knob) {
    return std::get<long long>(QueryKnobRegistry::instance().entry(knob.id).readGlobal());
}

}  // namespace

int64_t MemoryUsageLimit::_resolveAndLatch(OperationContext* opCtx) const {
    const auto& knob = std::get<QueryKnob<long long>>(_slot);
    if (MONGO_unlikely(opCtx == nullptr)) {
        // No operation to resolve against. This only happens outside client operations, on
        // long-lived detached ExpressionContexts (e.g. collection validators). Read the knob's
        // global value without latching: latching here would pin the value for the lifetime of
        // the owning object.
        return readGlobalKnobValue(knob);
    }
    // Return the resolved local rather than re-reading the variant, so this cold path performs no
    // further discriminant checks either.
    int64_t value = QueryKnobConfiguration::get(opCtx).get(knob);
    _slot = value;
    return value;
}

}  // namespace mongo
