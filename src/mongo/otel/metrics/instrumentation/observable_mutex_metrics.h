// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/string_map.h"

#include <memory>

namespace mongo {

/**
 * Owns the OpenTelemetry counters for observable mutex contention metrics, grouped by mutex tag.
 * Per-tag counters are created lazily on the first collection cycle in which a tag appears.
 */
class ObservableMutexMetrics {
public:
    ObservableMutexMetrics();
    ~ObservableMutexMetrics();

    // This is not thread-safe. It relies on the periodic runner only ever invoking it from a single
    // thread; concurrent calls are not supported.
    void update(const StringMap<MutexStats>& statsPerTag);

private:
    class Impl;
    static otel::metrics::DynamicMetricNameMaker::Passkey dyn_metric_passkey() {
        return {};
    }
    std::unique_ptr<Impl> _impl;
};

[[MONGO_MOD_PUBLIC]] void installObservableMutexMetrics(ServiceContext* svcCtx);

}  // namespace mongo
