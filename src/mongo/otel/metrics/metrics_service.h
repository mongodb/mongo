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
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/otel/metrics/otel_metric_name_validation.h"
#include "mongo/otel/metrics/server_status_metric_name_validation.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <memory>
#include <typeindex>
#include <vector>

#include <absl/container/btree_map.h>
#include <boost/optional.hpp>
#include <fmt/format.h>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/provider.h>
#endif  // MONGO_CONFIG_OTEL

MONGO_MOD_PUBLIC;
namespace mongo::otel::metrics {

struct ScalarMetricOptions {
    boost::optional<ServerStatusOptions> serverStatusOptions = boost::none;
};
using CounterOptions = ScalarMetricOptions;
using UpDownCounterOptions = ScalarMetricOptions;
using GaugeOptions = ScalarMetricOptions;

struct HistogramOptions {
    boost::optional<ServerStatusOptions> serverStatusOptions = boost::none;
    /**
     * explicitBucketBoundaries allows users to specify custom buckets. The vector elements
     * denote the upper and lower bounds for the histogram buckets.
     *
     * Bucket upper-bounds are inclusive (except when the upper-bound is +inf), and bucket
     * lower-bounds are exclusive. The implicit first boundary is -inf and the implicit last
     * boundary is +inf. Given a list of n boundaries, there are n + 1 buckets. For example,
     *
     * boundaries = {2, 4}
     * buckets = (-inf, 2], (2, 4], (4, +inf)
     *
     * If, for example, the value 2 is recorded, the corresponding counts for each bucket would
     * be {1, 0, 0}.
     *
     * If a value is not provided, the default bucket boundaries will be used: {0, 5, 10, 25,
     * 50, 75, 100, 250, 500, 750, 1000, 2500, 5000, 7500, 10000}.
     *
     * See https://opentelemetry.io/docs/specs/otel/metrics/data-model/#histogram for more
     * information.
     */
    boost::optional<std::vector<double>> explicitBucketBoundaries;
};

/**
 * The MetricsService is the external interface by which API consumers can create Instruments. The
 * MetricsService must be initialized before metrics can record values or be read.
 */
class MetricsService final {
public:
    static constexpr StringData kMeterName = "mongodb";

    static MetricsService& instance() {
        static MetricsService metricsService;
        return metricsService;
    }

    /**
     * Creates an int64_t counter with the provided parameters. The function will throw an exception
     * if the counter would collide with an existing metric (i.e., same name but different type or
     * other parameters). Metrics should be stashed once they are created to avoid taking a lock on
     * the global list of metrics in performance-sensitive codepaths. Note that this will take
     * storage proportional to all possible attribute combinations, and will throw an exception if
     * there are too many.
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    template <AttributeType... AttributeTs>
    Counter<int64_t, AttributeTs...>& createInt64Counter(
        MetricName name,
        std::string description,
        MetricUnit unit,
        const AttributeDefinition<AttributeTs>&... defs,
        const CounterOptions& = {});

    /**
     * Creates a double counter with the provided parameters. The function will throw an exception
     * if the counter would collide with an existing metric (i.e., same name but different type or
     * other parameters). Metrics should be stashed once they are created to avoid taking a lock on
     * the global list of metrics in performance-sensitive codepaths. Note that this will take
     * storage proportional to all possible attribute combinations, and will throw an exception if
     * there are too many.
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    template <AttributeType... AttributeTs>
    Counter<double, AttributeTs...>& createDoubleCounter(
        MetricName name,
        std::string description,
        MetricUnit unit,
        const AttributeDefinition<AttributeTs>&... defs,
        const CounterOptions& = {});

    /**
     * Creates an int64_t up-down counter with the provided parameters. The function will throw an
     * exception if the instrument would collide with an existing metric (i.e., same name but
     * different type or other parameters). Metrics should be stashed once they are created to avoid
     * taking a lock on the global list of metrics in performance-sensitive codepaths.
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    UpDownCounter<int64_t>& createInt64UpDownCounter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit,
                                                     const UpDownCounterOptions& = {});

    /**
     * Creates a double up-down counter with the provided parameters. The function will throw an
     * exception if the instrument would collide with an existing metric (i.e., same name but
     * different type or other parameters). Metrics should be stashed once they are created to avoid
     * taking a lock on the global list of metrics in performance-sensitive codepaths.
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    UpDownCounter<double>& createDoubleUpDownCounter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit,
                                                     const UpDownCounterOptions& = {});

    /**
     * Creates or returns an existing gauge with the provided parameters. The function will throw an
     * exception if the gauge would collide with an different metric (i.e., same name but different
     * type or other parameters).
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    Gauge<int64_t>& createInt64Gauge(MetricName name,
                                     std::string description,
                                     MetricUnit unit,
                                     const GaugeOptions& = {});

    /**
     * Creates or returns an existing gauge with the provided parameters. The function will throw an
     * exception if the gauge would collide with an different metric (i.e., same name but different
     * type or other parameters).
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    Gauge<double>& createDoubleGauge(MetricName name,
                                     std::string description,
                                     MetricUnit unit,
                                     const GaugeOptions& = {});

    /**
     * Creates an int64_t histogram with the provided parameters. The function will throw an
     * exception if the counter would collide with an existing metric (i.e., same name but different
     * type or other parameters). Metrics should be stashed once they are created to avoid taking a
     * lock on the global list of metrics in performance-sensitive codepaths.
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    Histogram<int64_t>& createInt64Histogram(MetricName name,
                                             std::string description,
                                             MetricUnit unit,
                                             const HistogramOptions& options = {});

    /**
     * Creates a double histogram with the provided parameters. The function will throw an exception
     * if the counter would collide with an existing metric (i.e., same name but different type or
     * other parameters). Metrics should be stashed once they are created to avoid taking a lock on
     * the global list of metrics in performance-sensitive codepaths.
     *
     * All callers must add an entry in metric_names.h to create a MetricName to pass to the API.
     */
    Histogram<double>& createDoubleHistogram(MetricName name,
                                             std::string description,
                                             MetricUnit unit,
                                             const HistogramOptions& options = {});

    /**
     * Used in unit tests only. Removes all metrics registered by this MetricsService from the
     * internal map and from the serverStatus metric trees.
     */
    void clearForTests();

#ifdef MONGO_CONFIG_OTEL
    /**
     * Initializes the metrics service by registering metrics created before initialization with the
     * provided meter provider.
     *
     * This function enables metrics to be created statically, before the meter provider is
     * initialized in mongod_main.cpp. Any metrics created before initialization are reset to their
     * default value.
     */
    MONGO_MOD_PRIVATE void initialize(opentelemetry::metrics::MeterProvider& provider);
#endif  // MONGO_CONFIG_OTEL

private:
    /** Identifies metrics to help prevent conflicting registrations. */
    struct MetricIdentifier {
        std::string description;
        MetricUnit unit;
        /** The type index of the implementation class, which includes the attribute types. */
        std::type_index typeIndex = typeid(void);
        boost::optional<ServerStatusOptions> serverStatusOptions = boost::none;
        boost::optional<std::vector<double>> histogramBucketBoundaries = boost::none;
        /**
         * Names and formatted possible values for each attribute, in definition order.
         * NOTE: This duplicates attribute values also held by the metric implementation. If needed,
         * this could be removed by storing the attribute values in a centralized place and using
         * them both here and in the metric implementation.
         */
        std::vector<ComparableAttributeDefinition> attributeDefinitions;

        auto operator<=>(const MetricIdentifier& other) const = default;
    };

    template <typename InstrumentT>
    using MakeInstrumentRef =
        function_ref<std::unique_ptr<InstrumentT>(WithLock, const std::string& nameStr)>;

    template <typename InstrumentT>
    using AddObservableRef =
        function_ref<void(WithLock, const std::string& nameStr, InstrumentT* instrumentPtr)>;

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
    template <typename ImplT, typename OwnedMetricT>
    ImplT* _getDuplicateMetric(WithLock, const std::string& name, MetricIdentifier identifier);

    /**
     * Creates a counter with attributes of type AttributeTs and records values of type T. This
     * includes adding it to the appropriate internal data structures.
     */
    template <typename T, AttributeType... AttributeTs>
    Counter<T, AttributeTs...>& _createCounter(MetricName name,
                                               std::string description,
                                               MetricUnit unit,
                                               const AttributeDefinition<AttributeTs>&... defs,
                                               const CounterOptions& options);

    template <typename T>
    UpDownCounter<T>& createUpDownCounter(MetricName name,
                                          std::string description,
                                          MetricUnit unit,
                                          const UpDownCounterOptions& options);

    template <typename T>
    Gauge<T>& createGauge(MetricName name,
                          std::string description,
                          MetricUnit unit,
                          const GaugeOptions& options);

    template <typename T>
    Histogram<T>& createHistogram(MetricName name,
                                  std::string description,
                                  MetricUnit unit,
                                  const HistogramOptions& options);

    /**
     * If `serverStatusOptions` is specified, registers the given metric onto one of the
     * serverStatus metric trees based on the specified dotted path and role. Currently, the
     * registration is not thread-safe at runtime as the metric trees are not guarded by a mutex.
     * Therefore, `serverStatusOptions` must not be specified when creating a metric at runtime.
     * TODO (SERVER-123241): Make this thread-safe.
     */
    void _registerServerStatusTree(WithLock,
                                   Metric* metricPtr,
                                   const boost::optional<ServerStatusOptions>& serverStatusOptions);

    /**
     * Validates the given options. Then, either returns an existing matching metric or creates
     * one with `makeInstrument` and stores it. Then, registers the serverStatus tree if requested
     * and optionally runs `addObservable` if OTEL is enabled. ImplT is the implementation class and
     * OwnedMetricT is the type used for storage in _metrics and observation (where applicable).
     */
    template <typename ImplT, typename OwnedMetricT, typename Options>
    ImplT& _createMetric(MetricName name,
                         const Options& options,
                         MetricIdentifier identifier,
                         MakeInstrumentRef<ImplT> makeInstrument,
                         AddObservableRef<ImplT> addObservable);

#ifdef MONGO_CONFIG_OTEL
    /**
     * Registers an observable counter instrument with the OTEL provider and stores the resulting
     * handle in `_observableInstruments`.
     */
    template <template <typename> class MetricT, typename T>
    void _addObservable(WithLock,
                        const std::string& nameStr,
                        MetricT<T>* metricPtr,
                        std::string description,
                        MetricUnit unit);
#endif  // MONGO_CONFIG_OTEL

    using OwnedMetric = std::variant<std::unique_ptr<ObservableCounter<int64_t>>,
                                     std::unique_ptr<ObservableCounter<double>>,
                                     std::unique_ptr<UpDownCounter<int64_t>>,
                                     std::unique_ptr<UpDownCounter<double>>,
                                     std::unique_ptr<Gauge<int64_t>>,
                                     std::unique_ptr<Gauge<double>>,
                                     std::unique_ptr<Histogram<int64_t>>,
                                     std::unique_ptr<Histogram<double>>>;

#ifdef MONGO_CONFIG_OTEL
    /**
     * Visitor struct used for initializing OwnedMetric variants in MetricsService::initialize().
     *
     * The visitor assumes the MetricService mutex is locked, hence the WithLock member variable.
     */
    struct OwnedMetricVisitor {
        WithLock lock;
        opentelemetry::metrics::MeterProvider& provider;
        const std::string& name;
        MetricIdentifier id;
        std::vector<std::shared_ptr<opentelemetry::metrics::ObservableInstrument>>&
            newObservableInstruments;

        void operator()(std::unique_ptr<ObservableCounter<int64_t>>& counter);
        void operator()(std::unique_ptr<ObservableCounter<double>>& counter);
        void operator()(std::unique_ptr<UpDownCounter<int64_t>>& upDownCounter);
        void operator()(std::unique_ptr<UpDownCounter<double>>& upDownCounter);
        void operator()(std::unique_ptr<Gauge<int64_t>>& gauge);
        void operator()(std::unique_ptr<Gauge<double>>& gauge);
        void operator()(std::unique_ptr<Histogram<double>>& histogram);
        void operator()(std::unique_ptr<Histogram<int64_t>>& histogram);
    };
#endif  // MONGO_CONFIG_OTEL

    struct IdentifierAndMetric {
        MetricIdentifier identifier;
        OwnedMetric metric;
    };

    // Guards `_observableInstruments` and `_metrics`.
    mutable std::mutex _mutex;

#ifdef MONGO_CONFIG_OTEL
    // Pointers to all observable instruments. These are not directly used, but must be kept alive
    // while the instruments they back are still in use. Guarded by `_mutex`.
    std::vector<std::shared_ptr<opentelemetry::metrics::ObservableInstrument>>
        _observableInstruments;
#endif  // MONGO_CONFIG_OTEL

    // Map from metric name to its definition and implementation. Guarded by `_mutex`.
    absl::btree_map<std::string, IdentifierAndMetric> _metrics;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename ImplT, typename OwnedMetricT>
ImplT* MetricsService::_getDuplicateMetric(WithLock,
                                           const std::string& name,
                                           MetricIdentifier identifier) {
    if (auto it = _metrics.find(name); it != _metrics.end()) {
        massert(ErrorCodes::ObjectAlreadyExists,
                fmt::format("Tried to create a metric with the name: {} but different definition "
                            "already exists.",
                            name),
                it->second.identifier == identifier);
        massert(ErrorCodes::ObjectAlreadyExists,
                "Tried to create a new metric, but a metric with a different type parameter "
                "already exists.",
                std::holds_alternative<std::unique_ptr<OwnedMetricT>>(it->second.metric));

        return static_cast<ImplT*>(
            std::get<std::unique_ptr<OwnedMetricT>>(it->second.metric).get());
    }
    return nullptr;
}

template <typename ImplT, typename OwnedMetricT, typename Options>
ImplT& MetricsService::_createMetric(MetricName name,
                                     const Options& options,
                                     MetricIdentifier identifier,
                                     MakeInstrumentRef<ImplT> makeInstrument,
                                     AddObservableRef<ImplT> addObservable) {
    // Validate otel and serverStatus metric names.
    uassertStatusOK(validateOtelMetricName(name.getName()));
    if (options.serverStatusOptions) {
        uassertStatusOK(validateServerStatusMetricPath(options.serverStatusOptions->dottedPath));
    }

    const std::string nameStr(name.getName());

    std::lock_guard lock(_mutex);

    // Check for duplicate.
    if (auto* duplicate = _getDuplicateMetric<ImplT, OwnedMetricT>(lock, nameStr, identifier)) {
        return *duplicate;
    }

    // Register.
    auto impl = makeInstrument(lock, nameStr);
    ImplT* const ptr = impl.get();

    auto owned = OwnedMetric{std::unique_ptr<ImplT>(std::move(impl))};
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(owned)};
    _registerServerStatusTree(lock, static_cast<Metric*>(ptr), options.serverStatusOptions);
#ifdef MONGO_CONFIG_OTEL
    addObservable(lock, nameStr, ptr);
#endif  // MONGO_CONFIG_OTEL
    return *ptr;
}

template <typename T, AttributeType... AttributeTs>
Counter<T, AttributeTs...>& MetricsService::_createCounter(
    MetricName name,
    std::string description,
    MetricUnit unit,
    const AttributeDefinition<AttributeTs>&... defs,
    const CounterOptions& options) {
    MetricIdentifier identifier{
        .description = description,
        .unit = unit,
        .serverStatusOptions = options.serverStatusOptions,
        .histogramBucketBoundaries = boost::none,
        .attributeDefinitions = {makeComparableAttributeDefinition(defs)...}};
    CounterImpl<T, AttributeTs...>& base =
        _createMetric<CounterImpl<T, AttributeTs...>, ObservableCounter<T>, CounterOptions>(
            name,
            options,
            std::move(identifier),
            /* makeInstrument= */
            [&](WithLock, const std::string&) {
                return std::make_unique<CounterImpl<T, AttributeTs...>>(defs...);
            },
#ifdef MONGO_CONFIG_OTEL
            /* addObservable= */
            [this, desc = std::move(description), unit](
                WithLock lock,
                const std::string& nameStr,
                CounterImpl<T, AttributeTs...>* counterPtr) mutable {
                _addObservable(lock,
                               nameStr,
                               static_cast<ObservableCounter<T>*>(counterPtr),
                               std::move(desc),
                               unit);
            }
#else
            /* addObservable= */
            [](WithLock, const std::string&, CounterImpl<T, AttributeTs...>*) {}
#endif  // MONGO_CONFIG_OTEL
        );
    return base;
}

template <AttributeType... AttributeTs>
Counter<int64_t, AttributeTs...>& MetricsService::createInt64Counter(
    MetricName name,
    std::string description,
    MetricUnit unit,
    const AttributeDefinition<AttributeTs>&... defs,
    const CounterOptions& options) {
    return _createCounter<int64_t, AttributeTs...>(
        name, std::move(description), unit, defs..., options);
}

template <AttributeType... AttributeTs>
Counter<double, AttributeTs...>& MetricsService::createDoubleCounter(
    MetricName name,
    std::string description,
    MetricUnit unit,
    const AttributeDefinition<AttributeTs>&... defs,
    const CounterOptions& options) {
    return _createCounter<double, AttributeTs...>(
        name, std::move(description), unit, defs..., options);
}

#ifdef MONGO_CONFIG_OTEL
namespace metrics_service_detail {

template <template <typename> class MetricT, typename ValueT>
MONGO_MOD_FILE_PRIVATE void observableCallback(
    opentelemetry::metrics::ObserverResult observer_result, void* state) {
    invariant(state != nullptr);
    auto* const metric = static_cast<MetricT<ValueT>*>(state);
    auto observer = std::get_if<std::shared_ptr<opentelemetry::metrics::ObserverResultT<ValueT>>>(
        &observer_result);
    invariant(observer != nullptr && *observer != nullptr);
    for (const auto& entry : metric->values()) {
        (*observer)->Observe(entry.value, entry.attributes);
    }
}

template <typename T>
MONGO_MOD_FILE_PRIVATE std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument(opentelemetry::metrics::MeterProvider& provider,
                         std::string name,
                         std::string description,
                         MetricUnit unit);

template <>
MONGO_MOD_FILE_PRIVATE inline std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<ObservableCounter<int64_t>>(
    opentelemetry::metrics::MeterProvider& provider,
    std::string name,
    std::string description,
    MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableCounter(toStdStringViewForInterop(name),
                                       description,
                                       toStdStringViewForInterop(toString(unit)));
}

template <>
MONGO_MOD_FILE_PRIVATE inline std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<ObservableCounter<double>>(opentelemetry::metrics::MeterProvider& provider,
                                                    std::string name,
                                                    std::string description,
                                                    MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableCounter(toStdStringViewForInterop(name),
                                        description,
                                        toStdStringViewForInterop(toString(unit)));
}

template <>
MONGO_MOD_FILE_PRIVATE inline std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<UpDownCounter<int64_t>>(opentelemetry::metrics::MeterProvider& provider,
                                                 std::string name,
                                                 std::string description,
                                                 MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableUpDownCounter(toStdStringViewForInterop(name),
                                             description,
                                             toStdStringViewForInterop(toString(unit)));
}

template <>
MONGO_MOD_FILE_PRIVATE inline std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<UpDownCounter<double>>(opentelemetry::metrics::MeterProvider& provider,
                                                std::string name,
                                                std::string description,
                                                MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableUpDownCounter(toStdStringViewForInterop(name),
                                              description,
                                              toStdStringViewForInterop(toString(unit)));
}

template <>
MONGO_MOD_FILE_PRIVATE inline std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Gauge<int64_t>>(opentelemetry::metrics::MeterProvider& provider,
                                         std::string name,
                                         std::string description,
                                         MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableGauge(toStdStringViewForInterop(name),
                                     description,
                                     toStdStringViewForInterop(toString(unit)));
}

template <>
MONGO_MOD_FILE_PRIVATE inline std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Gauge<double>>(opentelemetry::metrics::MeterProvider& provider,
                                        std::string name,
                                        std::string description,
                                        MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableGauge(toStdStringViewForInterop(name),
                                      description,
                                      toStdStringViewForInterop(toString(unit)));
}
}  // namespace metrics_service_detail

template <template <typename> class MetricT, typename T>
void MetricsService::_addObservable(WithLock,
                                    const std::string& nameStr,
                                    MetricT<T>* metricPtr,
                                    std::string description,
                                    MetricUnit unit) {
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    auto observable = metrics_service_detail::makeObservableInstrument<MetricT<T>>(
        *provider, nameStr, description, unit);
    tassert(ErrorCodes::InternalError,
            fmt::format("Could not create observable instrument for metric: {}", nameStr),
            observable != nullptr);
    observable->AddCallback(metrics_service_detail::observableCallback<MetricT, T>, metricPtr);
    _observableInstruments.push_back(std::move(observable));
}
#endif  // MONGO_CONFIG_OTEL

}  // namespace mongo::otel::metrics
