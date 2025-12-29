/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/util/modules.h"

#include <absl/container/btree_map.h>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/provider.h>

namespace mongo::otel::metrics {

/**
 * The MetricsService is the external interface by which API consumers can create Instruments. The
 * global MeterProvider must be set before ServiceContext construction to ensure that the meter can
 * be properly initialized.
 */
class MONGO_MOD_PUBLIC MetricsService {
public:
    static constexpr StringData kMeterName = "mongodb";

    static MetricsService& get(ServiceContext*);

    /**
     * Creates a counter with the provided parameters. The result is never null but will throw an
     * exception if the counter would collide with an existing metric (i.e., same name but different
     * type or other parameters).
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    Counter<int64_t>* createInt64Counter(MetricName name, std::string description, MetricUnit unit);

    // TODO SERVER-114954 Implement MetricsService::createUInt64Gauge
    // TODO SERVER-114955 Implement MetricsService::createDoubleGauge

    /**
     * Creates an int64_t histogram with the provided parameters. The result is never null but will
     * throw an exception if the counter would collide with an existing metric (i.e., same name but
     * different type or other parameters).
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    Histogram<int64_t>* createInt64Histogram(MetricName name,
                                             std::string description,
                                             MetricUnit unit);

    /**
     * Creates a double histogram with the provided parameters. The result is never null but will
     * throw an exception if the counter would collide with an existing metric (i.e., same name but
     * different type or other parameters).
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    Histogram<double>* createDoubleHistogram(MetricName name,
                                             std::string description,
                                             MetricUnit unit);

    /**
     * Serializes the created metrics to BSON for server status reporting.
     */
    BSONObj serializeMetrics() const;

private:
    // Identifies metrics to help prevent conflicting registrations.
    struct MetricIdentifier {
        std::string description;
        MetricUnit unit;

        auto operator<=>(const MetricIdentifier& other) const = default;
    };

    /**
     * Retrieves an existing metric if one with the same name, identifier, and type T exists.
     *
     * This helper handles cases where developers may attempt to "create" the same metric multiple
     * times (e.g., when refactoring a class into multiple files with duplicated metric recording
     * logic). If a matching metric exists, a pointer to it is returned. Otherwise, returns nullptr.
     *
     * If a metric with the same name and different identifier or type T exists, an exception is
     * thrown.
     */
    template <typename T>
    T* getDuplicateMetric(WithLock, const std::string& name, MetricIdentifier identifier);

    using OwnedMetric = std::variant<std::unique_ptr<Counter<int64_t>>,
                                     std::unique_ptr<Histogram<double>>,
                                     std::unique_ptr<Histogram<int64_t>>>;

    struct IdentifierAndMetric {
        MetricIdentifier identifier;
        OwnedMetric metric;
    };

    // Guards `_observableInstruments` and `_metrics`.
    mutable stdx::mutex _mutex;

    // Pointers to all observable instruments. These are not directly used, but must be kept alive
    // while the instruments they back are still in use. Guarded by `_mutex`.
    std::vector<std::shared_ptr<opentelemetry::metrics::ObservableInstrument>>
        _observableInstruments;

    // Map from metric name to its definition and implementation. Guarded by `_mutex`.
    absl::btree_map<std::string, IdentifierAndMetric> _metrics;
};
}  // namespace mongo::otel::metrics
#else
namespace mongo::otel::metrics {
class MONGO_MOD_PUBLIC MetricsService {
public:
    static constexpr StringData kMeterName = "mongodb";

    static MetricsService& get(ServiceContext*);

    Counter<int64_t>* createInt64Counter(MetricName name, std::string description, MetricUnit unit);

    Histogram<int64_t>* createInt64Histogram(MetricName name,
                                             std::string description,
                                             MetricUnit unit);

    Histogram<double>* createDoubleHistogram(MetricName name,
                                             std::string description,
                                             MetricUnit unit);

private:
    stdx::mutex _mutex;
    std::vector<std::unique_ptr<Metric>> _metrics;
};
}  // namespace mongo::otel::metrics
#endif
