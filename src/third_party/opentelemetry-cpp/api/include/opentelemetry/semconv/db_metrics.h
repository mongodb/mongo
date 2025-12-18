/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_metrics-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace db
{

/**
  Duration of database client operations.
  <p>
  Batch operations SHOULD be recorded as a single operation.
  <p>
  histogram
 */
static constexpr const char *kMetricDbClientOperationDuration = "db.client.operation.duration";
static constexpr const char *descrMetricDbClientOperationDuration =
    "Duration of database client operations.";
static constexpr const char *unitMetricDbClientOperationDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientOperationDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientOperationDuration,
                                      descrMetricDbClientOperationDuration,
                                      unitMetricDbClientOperationDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientOperationDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientOperationDuration,
                                      descrMetricDbClientOperationDuration,
                                      unitMetricDbClientOperationDuration);
}

}  // namespace db
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
