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
namespace azure
{

/**
 * Number of active client instances
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricAzureCosmosdbClientActiveInstanceCount =
    "azure.cosmosdb.client.active_instance.count";
static constexpr const char *descrMetricAzureCosmosdbClientActiveInstanceCount =
    "Number of active client instances";
static constexpr const char *unitMetricAzureCosmosdbClientActiveInstanceCount = "{instance}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricAzureCosmosdbClientActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricAzureCosmosdbClientActiveInstanceCount,
                                         descrMetricAzureCosmosdbClientActiveInstanceCount,
                                         unitMetricAzureCosmosdbClientActiveInstanceCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricAzureCosmosdbClientActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricAzureCosmosdbClientActiveInstanceCount,
                                          descrMetricAzureCosmosdbClientActiveInstanceCount,
                                          unitMetricAzureCosmosdbClientActiveInstanceCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricAzureCosmosdbClientActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricAzureCosmosdbClientActiveInstanceCount,
      descrMetricAzureCosmosdbClientActiveInstanceCount,
      unitMetricAzureCosmosdbClientActiveInstanceCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricAzureCosmosdbClientActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricAzureCosmosdbClientActiveInstanceCount,
      descrMetricAzureCosmosdbClientActiveInstanceCount,
      unitMetricAzureCosmosdbClientActiveInstanceCount);
}

/**
 * <a href="https://learn.microsoft.com/azure/cosmos-db/request-units">Request units</a> consumed by
 * the operation <p> histogram
 */
static constexpr const char *kMetricAzureCosmosdbClientOperationRequestCharge =
    "azure.cosmosdb.client.operation.request_charge";
static constexpr const char *descrMetricAzureCosmosdbClientOperationRequestCharge =
    "[Request units](https://learn.microsoft.com/azure/cosmos-db/request-units) consumed by the "
    "operation";
static constexpr const char *unitMetricAzureCosmosdbClientOperationRequestCharge = "{request_unit}";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricAzureCosmosdbClientOperationRequestCharge(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricAzureCosmosdbClientOperationRequestCharge,
                                      descrMetricAzureCosmosdbClientOperationRequestCharge,
                                      unitMetricAzureCosmosdbClientOperationRequestCharge);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricAzureCosmosdbClientOperationRequestCharge(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricAzureCosmosdbClientOperationRequestCharge,
                                      descrMetricAzureCosmosdbClientOperationRequestCharge,
                                      unitMetricAzureCosmosdbClientOperationRequestCharge);
}

}  // namespace azure
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
