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
namespace vcs
{

/**
 * The number of changes (pull requests/merge requests/changelists) in a repository, categorized by
 * their state (e.g. open or merged) <p> updowncounter
 */
static constexpr const char *kMetricVcsChangeCount = "vcs.change.count";
static constexpr const char *descrMetricVcsChangeCount =
    "The number of changes (pull requests/merge requests/changelists) in a repository, categorized "
    "by their state (e.g. open or merged)";
static constexpr const char *unitMetricVcsChangeCount = "{change}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricVcsChangeCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricVcsChangeCount, descrMetricVcsChangeCount,
                                         unitMetricVcsChangeCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricVcsChangeCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricVcsChangeCount, descrMetricVcsChangeCount,
                                          unitMetricVcsChangeCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricVcsChangeCount(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricVcsChangeCount, descrMetricVcsChangeCount,
                                                   unitMetricVcsChangeCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsChangeCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricVcsChangeCount, descrMetricVcsChangeCount, unitMetricVcsChangeCount);
}

/**
 * The time duration a change (pull request/merge request/changelist) has been in a given state.
 * <p>
 * gauge
 */
static constexpr const char *kMetricVcsChangeDuration = "vcs.change.duration";
static constexpr const char *descrMetricVcsChangeDuration =
    "The time duration a change (pull request/merge request/changelist) has been in a given state.";
static constexpr const char *unitMetricVcsChangeDuration = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricVcsChangeDuration(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricVcsChangeDuration, descrMetricVcsChangeDuration,
                                 unitMetricVcsChangeDuration);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricVcsChangeDuration(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricVcsChangeDuration, descrMetricVcsChangeDuration,
                                  unitMetricVcsChangeDuration);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricVcsChangeDuration(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricVcsChangeDuration, descrMetricVcsChangeDuration,
                                           unitMetricVcsChangeDuration);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsChangeDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricVcsChangeDuration, descrMetricVcsChangeDuration,
                                            unitMetricVcsChangeDuration);
}

/**
 * The amount of time since its creation it took a change (pull request/merge request/changelist) to
 * get the first approval. <p> gauge
 */
static constexpr const char *kMetricVcsChangeTimeToApproval = "vcs.change.time_to_approval";
static constexpr const char *descrMetricVcsChangeTimeToApproval =
    "The amount of time since its creation it took a change (pull request/merge "
    "request/changelist) to get the first approval.";
static constexpr const char *unitMetricVcsChangeTimeToApproval = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricVcsChangeTimeToApproval(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricVcsChangeTimeToApproval, descrMetricVcsChangeTimeToApproval,
                                 unitMetricVcsChangeTimeToApproval);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricVcsChangeTimeToApproval(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricVcsChangeTimeToApproval,
                                  descrMetricVcsChangeTimeToApproval,
                                  unitMetricVcsChangeTimeToApproval);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricVcsChangeTimeToApproval(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricVcsChangeTimeToApproval,
                                           descrMetricVcsChangeTimeToApproval,
                                           unitMetricVcsChangeTimeToApproval);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsChangeTimeToApproval(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricVcsChangeTimeToApproval,
                                            descrMetricVcsChangeTimeToApproval,
                                            unitMetricVcsChangeTimeToApproval);
}

/**
 * The amount of time since its creation it took a change (pull request/merge request/changelist) to
 * get merged into the target(base) ref. <p> gauge
 */
static constexpr const char *kMetricVcsChangeTimeToMerge = "vcs.change.time_to_merge";
static constexpr const char *descrMetricVcsChangeTimeToMerge =
    "The amount of time since its creation it took a change (pull request/merge "
    "request/changelist) to get merged into the target(base) ref.";
static constexpr const char *unitMetricVcsChangeTimeToMerge = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricVcsChangeTimeToMerge(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricVcsChangeTimeToMerge, descrMetricVcsChangeTimeToMerge,
                                 unitMetricVcsChangeTimeToMerge);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricVcsChangeTimeToMerge(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricVcsChangeTimeToMerge, descrMetricVcsChangeTimeToMerge,
                                  unitMetricVcsChangeTimeToMerge);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricVcsChangeTimeToMerge(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(
      kMetricVcsChangeTimeToMerge, descrMetricVcsChangeTimeToMerge, unitMetricVcsChangeTimeToMerge);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsChangeTimeToMerge(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricVcsChangeTimeToMerge, descrMetricVcsChangeTimeToMerge, unitMetricVcsChangeTimeToMerge);
}

/**
 * The number of unique contributors to a repository
 * <p>
 * gauge
 */
static constexpr const char *kMetricVcsContributorCount = "vcs.contributor.count";
static constexpr const char *descrMetricVcsContributorCount =
    "The number of unique contributors to a repository";
static constexpr const char *unitMetricVcsContributorCount = "{contributor}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricVcsContributorCount(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricVcsContributorCount, descrMetricVcsContributorCount,
                                 unitMetricVcsContributorCount);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricVcsContributorCount(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricVcsContributorCount, descrMetricVcsContributorCount,
                                  unitMetricVcsContributorCount);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricVcsContributorCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(
      kMetricVcsContributorCount, descrMetricVcsContributorCount, unitMetricVcsContributorCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsContributorCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricVcsContributorCount, descrMetricVcsContributorCount, unitMetricVcsContributorCount);
}

/**
 * The number of refs of type branch or tag in a repository.
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricVcsRefCount = "vcs.ref.count";
static constexpr const char *descrMetricVcsRefCount =
    "The number of refs of type branch or tag in a repository.";
static constexpr const char *unitMetricVcsRefCount = "{ref}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricVcsRefCount(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricVcsRefCount, descrMetricVcsRefCount,
                                         unitMetricVcsRefCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricVcsRefCount(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricVcsRefCount, descrMetricVcsRefCount,
                                          unitMetricVcsRefCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricVcsRefCount(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricVcsRefCount, descrMetricVcsRefCount,
                                                   unitMetricVcsRefCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricVcsRefCount(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricVcsRefCount, descrMetricVcsRefCount,
                                                    unitMetricVcsRefCount);
}

/**
 * The number of lines added/removed in a ref (branch) relative to the ref from the @code
 * vcs.ref.base.name @endcode attribute. <p> This metric should be reported for each @code
 * vcs.line_change.type @endcode value. For example if a ref added 3 lines and removed 2 lines,
 * instrumentation SHOULD report two measurements: 3 and 2 (both positive numbers).
 * If number of lines added/removed should be calculated from the start of time, then @code
 * vcs.ref.base.name @endcode SHOULD be set to an empty string. <p> gauge
 */
static constexpr const char *kMetricVcsRefLinesDelta = "vcs.ref.lines_delta";
static constexpr const char *descrMetricVcsRefLinesDelta =
    "The number of lines added/removed in a ref (branch) relative to the ref from the "
    "`vcs.ref.base.name` attribute.";
static constexpr const char *unitMetricVcsRefLinesDelta = "{line}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricVcsRefLinesDelta(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricVcsRefLinesDelta, descrMetricVcsRefLinesDelta,
                                 unitMetricVcsRefLinesDelta);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricVcsRefLinesDelta(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricVcsRefLinesDelta, descrMetricVcsRefLinesDelta,
                                  unitMetricVcsRefLinesDelta);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricVcsRefLinesDelta(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricVcsRefLinesDelta, descrMetricVcsRefLinesDelta,
                                           unitMetricVcsRefLinesDelta);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsRefLinesDelta(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricVcsRefLinesDelta, descrMetricVcsRefLinesDelta,
                                            unitMetricVcsRefLinesDelta);
}

/**
 * The number of revisions (commits) a ref (branch) is ahead/behind the branch from the @code
 * vcs.ref.base.name @endcode attribute <p> This metric should be reported for each @code
 * vcs.revision_delta.direction @endcode value. For example if branch @code a @endcode is 3 commits
 * behind and 2 commits ahead of @code trunk @endcode, instrumentation SHOULD report two
 * measurements: 3 and 2 (both positive numbers) and @code vcs.ref.base.name @endcode is set to
 * @code trunk @endcode. <p> gauge
 */
static constexpr const char *kMetricVcsRefRevisionsDelta = "vcs.ref.revisions_delta";
static constexpr const char *descrMetricVcsRefRevisionsDelta =
    "The number of revisions (commits) a ref (branch) is ahead/behind the branch from the "
    "`vcs.ref.base.name` attribute";
static constexpr const char *unitMetricVcsRefRevisionsDelta = "{revision}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricVcsRefRevisionsDelta(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricVcsRefRevisionsDelta, descrMetricVcsRefRevisionsDelta,
                                 unitMetricVcsRefRevisionsDelta);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricVcsRefRevisionsDelta(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricVcsRefRevisionsDelta, descrMetricVcsRefRevisionsDelta,
                                  unitMetricVcsRefRevisionsDelta);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricVcsRefRevisionsDelta(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(
      kMetricVcsRefRevisionsDelta, descrMetricVcsRefRevisionsDelta, unitMetricVcsRefRevisionsDelta);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsRefRevisionsDelta(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricVcsRefRevisionsDelta, descrMetricVcsRefRevisionsDelta, unitMetricVcsRefRevisionsDelta);
}

/**
 * Time a ref (branch) created from the default branch (trunk) has existed. The @code ref.type
 * @endcode attribute will always be @code branch @endcode <p> gauge
 */
static constexpr const char *kMetricVcsRefTime = "vcs.ref.time";
static constexpr const char *descrMetricVcsRefTime =
    "Time a ref (branch) created from the default branch (trunk) has existed. The `ref.type` "
    "attribute will always be `branch`";
static constexpr const char *unitMetricVcsRefTime = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricVcsRefTime(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricVcsRefTime, descrMetricVcsRefTime, unitMetricVcsRefTime);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricVcsRefTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricVcsRefTime, descrMetricVcsRefTime, unitMetricVcsRefTime);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricVcsRefTime(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricVcsRefTime, descrMetricVcsRefTime,
                                           unitMetricVcsRefTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricVcsRefTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricVcsRefTime, descrMetricVcsRefTime,
                                            unitMetricVcsRefTime);
}

/**
 * The number of repositories in an organization.
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricVcsRepositoryCount = "vcs.repository.count";
static constexpr const char *descrMetricVcsRepositoryCount =
    "The number of repositories in an organization.";
static constexpr const char *unitMetricVcsRepositoryCount = "{repository}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricVcsRepositoryCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricVcsRepositoryCount, descrMetricVcsRepositoryCount,
                                         unitMetricVcsRepositoryCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricVcsRepositoryCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricVcsRepositoryCount, descrMetricVcsRepositoryCount,
                                          unitMetricVcsRepositoryCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricVcsRepositoryCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricVcsRepositoryCount, descrMetricVcsRepositoryCount, unitMetricVcsRepositoryCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricVcsRepositoryCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricVcsRepositoryCount, descrMetricVcsRepositoryCount, unitMetricVcsRepositoryCount);
}

}  // namespace vcs
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
