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
namespace dns
{

/**
 * Measures the time taken to perform a DNS lookup.
 * <p>
 * histogram
 */
static constexpr const char *kMetricDnsLookupDuration = "dns.lookup.duration";
static constexpr const char *descrMetricDnsLookupDuration =
    "Measures the time taken to perform a DNS lookup.";
static constexpr const char *unitMetricDnsLookupDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDnsLookupDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDnsLookupDuration, descrMetricDnsLookupDuration,
                                      unitMetricDnsLookupDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>> CreateSyncDoubleMetricDnsLookupDuration(
    metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDnsLookupDuration, descrMetricDnsLookupDuration,
                                      unitMetricDnsLookupDuration);
}

}  // namespace dns
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
