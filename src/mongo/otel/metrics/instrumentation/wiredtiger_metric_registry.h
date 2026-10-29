// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mongo {

/**
 * Translates a WiredTiger statistic name into a valid otel metric name, e.g.
 * "cache: bytes read into cache" becomes
 * "mongodb.serverStatus.wiredTiger.cache.bytes_read_into_cache".
 *
 * Stat names that cannot be translated into a valid otel metric name are remembered and skipped on
 * later cycles.
 */
[[MONGO_MOD_PUBLIC]] std::string wtStatNameToOtelName(std::string_view statName);

/**
 * Lazily creates and owns the OpenTelemetry instruments backing WiredTiger statistics, keyed by the
 * WiredTiger stat name.
 *
 * This is not thread-safe. It relies on the collection job only ever invoking it from a single
 * thread. Concurrent calls are not supported.
 */
class WiredTigerMetricRegistry {
public:
    /**
     * Records an increase in an OTel counter metric. WiredTiger reports counters as the increment
     * since the last reading, which is what the OTel counter API expects, so the value is passed
     * through unchanged. The increment must be non-negative.
     */
    void updateCounter(std::string_view statName, int64_t increment);

    /**
     * Updates an OTel gauge metric to reflect the new currentValue. Gauges represent point-in-time
     * values.
     */
    void updateGauge(std::string_view statName, int64_t currentValue);

private:
    static otel::metrics::DynamicMetricNameMaker::Passkey dyn_metric_passkey() {
        return {};
    }

    otel::metrics::Counter<int64_t>* _createCounter(std::string_view statName);
    otel::metrics::Gauge<int64_t>* _createGauge(std::string_view statName);

    StringMap<otel::metrics::Counter<int64_t>*> _counters;
    StringMap<otel::metrics::Gauge<int64_t>*> _gauges;
    StringSet _skipped;
};

}  // namespace mongo
