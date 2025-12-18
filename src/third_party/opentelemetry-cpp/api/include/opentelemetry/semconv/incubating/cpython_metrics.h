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
namespace cpython
{

/**
  The total number of objects collected inside a generation since interpreter start.
  <p>
  This metric reports data from <a
  href="https://docs.python.org/3/library/gc.html#gc.get_stats">@code gc.stats() @endcode</a>. <p>
  counter
 */
static constexpr const char *kMetricCpythonGcCollectedObjects = "cpython.gc.collected_objects";
static constexpr const char *descrMetricCpythonGcCollectedObjects =
    "The total number of objects collected inside a generation since interpreter start.";
static constexpr const char *unitMetricCpythonGcCollectedObjects = "{object}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricCpythonGcCollectedObjects(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricCpythonGcCollectedObjects,
                                    descrMetricCpythonGcCollectedObjects,
                                    unitMetricCpythonGcCollectedObjects);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricCpythonGcCollectedObjects(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricCpythonGcCollectedObjects,
                                    descrMetricCpythonGcCollectedObjects,
                                    unitMetricCpythonGcCollectedObjects);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricCpythonGcCollectedObjects(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricCpythonGcCollectedObjects,
                                             descrMetricCpythonGcCollectedObjects,
                                             unitMetricCpythonGcCollectedObjects);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricCpythonGcCollectedObjects(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricCpythonGcCollectedObjects,
                                              descrMetricCpythonGcCollectedObjects,
                                              unitMetricCpythonGcCollectedObjects);
}

/**
  The number of times a generation was collected since interpreter start.
  <p>
  This metric reports data from <a
  href="https://docs.python.org/3/library/gc.html#gc.get_stats">@code gc.stats() @endcode</a>. <p>
  counter
 */
static constexpr const char *kMetricCpythonGcCollections = "cpython.gc.collections";
static constexpr const char *descrMetricCpythonGcCollections =
    "The number of times a generation was collected since interpreter start.";
static constexpr const char *unitMetricCpythonGcCollections = "{collection}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricCpythonGcCollections(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricCpythonGcCollections, descrMetricCpythonGcCollections,
                                    unitMetricCpythonGcCollections);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricCpythonGcCollections(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricCpythonGcCollections, descrMetricCpythonGcCollections,
                                    unitMetricCpythonGcCollections);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricCpythonGcCollections(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricCpythonGcCollections, descrMetricCpythonGcCollections, unitMetricCpythonGcCollections);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricCpythonGcCollections(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricCpythonGcCollections, descrMetricCpythonGcCollections, unitMetricCpythonGcCollections);
}

/**
  The total number of objects which were found to be uncollectable inside a generation since
  interpreter start. <p> This metric reports data from <a
  href="https://docs.python.org/3/library/gc.html#gc.get_stats">@code gc.stats() @endcode</a>. <p>
  counter
 */
static constexpr const char *kMetricCpythonGcUncollectableObjects =
    "cpython.gc.uncollectable_objects";
static constexpr const char *descrMetricCpythonGcUncollectableObjects =
    "The total number of objects which were found to be uncollectable inside a generation since "
    "interpreter start.";
static constexpr const char *unitMetricCpythonGcUncollectableObjects = "{object}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricCpythonGcUncollectableObjects(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricCpythonGcUncollectableObjects,
                                    descrMetricCpythonGcUncollectableObjects,
                                    unitMetricCpythonGcUncollectableObjects);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricCpythonGcUncollectableObjects(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricCpythonGcUncollectableObjects,
                                    descrMetricCpythonGcUncollectableObjects,
                                    unitMetricCpythonGcUncollectableObjects);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricCpythonGcUncollectableObjects(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricCpythonGcUncollectableObjects,
                                             descrMetricCpythonGcUncollectableObjects,
                                             unitMetricCpythonGcUncollectableObjects);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricCpythonGcUncollectableObjects(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricCpythonGcUncollectableObjects,
                                              descrMetricCpythonGcUncollectableObjects,
                                              unitMetricCpythonGcUncollectableObjects);
}

}  // namespace cpython
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
