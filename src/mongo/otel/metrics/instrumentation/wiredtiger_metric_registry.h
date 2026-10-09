// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mongo {

/**
 * Translates a WiredTiger statistic name into a valid otel metric name, e.g.
 * "cache: bytes read into cache" becomes
 * "mongodb.serverStatus.wiredTiger.cache.bytes_read_into_cache".
 */
[[MONGO_MOD_PUBLIC]] std::string wtStatNameToOtelName(std::string_view statName);

/**
 * Creates and owns the OpenTelemetry instruments backing WiredTiger statistics.
 *
 * This is not thread-safe. It relies on the collection job only ever invoking it from a single
 * thread. Concurrent calls are not supported.
 */
class WiredTigerMetricRegistry {
private:
    static otel::metrics::DynamicMetricNameMaker::Passkey dyn_metric_passkey() {
        return {};
    }

    otel::metrics::Counter<int64_t>* _createCounter(const std::string& statName);
    otel::metrics::Gauge<int64_t>* _createGauge(const std::string& statName);
};

}  // namespace mongo
