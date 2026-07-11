// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"
#include "mongo/util/modules.h"

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>
#endif  // MONGO_CONFIG_OTEL

namespace mongo::otel::metrics {

/** Controls when a metric value is included in values() output. */
enum class ReportingPolicy {
    kIfEverNonZero,       // Report if the value has ever been non-zero.
    kIfCurrentlyNonZero,  // Report if the value is non-zero.
    kUnconditionally,     // Report regardless of the value.
};

/** Controls how histogram metric is serialized by serializeToBson(). */
enum class [[MONGO_MOD_PUBLIC]] HistogramSerializationFormat {
    kAverage,       ///< Report exponential moving average and total count.
    kBucketCounts,  ///< Report per-bucket counts and total count.
};

/**
 * An abstract class for operations that are common among all metrics.
 */
class Metric {
public:
    virtual ~Metric() = default;

    /**
     * Serializes the derived class to BSON.
     *
     * This is useful for reporting metrics through server status. Implementations MUST use the
     * provided key to generate a BSONOBj with the following structure:
     *
     * {key: ...}
     *
     * For example, {"a": 1} or {"b": {...}}.
     */
    virtual BSONObj serializeToBson(const std::string& key) const = 0;

#ifdef MONGO_CONFIG_OTEL
    /**
     * Resets the metric to its original state.
     *
     * This is used during MetricsService initialization since recording values pre-init should be
     * a no-op.
     */
    virtual void reset(opentelemetry::metrics::Meter* meter = nullptr) = 0;
#endif  // MONGO_CONFIG_OTEL
};
}  // namespace mongo::otel::metrics
