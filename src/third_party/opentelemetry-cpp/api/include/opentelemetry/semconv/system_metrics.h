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
namespace system
{

/**
 * Deprecated. Use @code cpu.frequency @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code cpu.frequency @endcode.", "reason": "uncategorized"}
 * <p>
 * gauge
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricSystemCpuFrequency =
    "system.cpu.frequency";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricSystemCpuFrequency =
    "Deprecated. Use `cpu.frequency` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricSystemCpuFrequency = "{Hz}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricSystemCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricSystemCpuFrequency, descrMetricSystemCpuFrequency,
                                 unitMetricSystemCpuFrequency);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricSystemCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricSystemCpuFrequency, descrMetricSystemCpuFrequency,
                                  unitMetricSystemCpuFrequency);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricSystemCpuFrequency, descrMetricSystemCpuFrequency,
                                           unitMetricSystemCpuFrequency);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricSystemCpuFrequency, descrMetricSystemCpuFrequency, unitMetricSystemCpuFrequency);
}

/**
 * Reports the number of logical (virtual) processor cores created by the operating system to manage
 * multitasking <p> Calculated by multiplying the number of sockets by the number of cores per
 * socket, and then by the number of threads per core <p> updowncounter
 */
static constexpr const char *kMetricSystemCpuLogicalCount = "system.cpu.logical.count";
static constexpr const char *descrMetricSystemCpuLogicalCount =
    "Reports the number of logical (virtual) processor cores created by the operating system to "
    "manage multitasking";
static constexpr const char *unitMetricSystemCpuLogicalCount = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemCpuLogicalCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemCpuLogicalCount,
                                         descrMetricSystemCpuLogicalCount,
                                         unitMetricSystemCpuLogicalCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemCpuLogicalCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemCpuLogicalCount,
                                          descrMetricSystemCpuLogicalCount,
                                          unitMetricSystemCpuLogicalCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemCpuLogicalCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricSystemCpuLogicalCount,
                                                   descrMetricSystemCpuLogicalCount,
                                                   unitMetricSystemCpuLogicalCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemCpuLogicalCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricSystemCpuLogicalCount,
                                                    descrMetricSystemCpuLogicalCount,
                                                    unitMetricSystemCpuLogicalCount);
}

/**
 * Reports the number of actual physical processor cores on the hardware
 * <p>
 * Calculated by multiplying the number of sockets by the number of cores per socket
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricSystemCpuPhysicalCount = "system.cpu.physical.count";
static constexpr const char *descrMetricSystemCpuPhysicalCount =
    "Reports the number of actual physical processor cores on the hardware";
static constexpr const char *unitMetricSystemCpuPhysicalCount = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemCpuPhysicalCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemCpuPhysicalCount,
                                         descrMetricSystemCpuPhysicalCount,
                                         unitMetricSystemCpuPhysicalCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemCpuPhysicalCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemCpuPhysicalCount,
                                          descrMetricSystemCpuPhysicalCount,
                                          unitMetricSystemCpuPhysicalCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemCpuPhysicalCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricSystemCpuPhysicalCount,
                                                   descrMetricSystemCpuPhysicalCount,
                                                   unitMetricSystemCpuPhysicalCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemCpuPhysicalCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricSystemCpuPhysicalCount,
                                                    descrMetricSystemCpuPhysicalCount,
                                                    unitMetricSystemCpuPhysicalCount);
}

/**
 * Deprecated. Use @code cpu.time @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code cpu.time @endcode.", "reason": "uncategorized"}
 * <p>
 * counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricSystemCpuTime = "system.cpu.time";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricSystemCpuTime =
    "Deprecated. Use `cpu.time` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricSystemCpuTime = "s";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemCpuTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemCpuTime, descrMetricSystemCpuTime,
                                    unitMetricSystemCpuTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricSystemCpuTime(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemCpuTime, descrMetricSystemCpuTime,
                                    unitMetricSystemCpuTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemCpuTime(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricSystemCpuTime, descrMetricSystemCpuTime,
                                             unitMetricSystemCpuTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemCpuTime(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricSystemCpuTime, descrMetricSystemCpuTime,
                                              unitMetricSystemCpuTime);
}

/**
 * Deprecated. Use @code cpu.utilization @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code cpu.utilization @endcode.", "reason": "uncategorized"}
 * <p>
 * gauge
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricSystemCpuUtilization =
    "system.cpu.utilization";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricSystemCpuUtilization =
    "Deprecated. Use `cpu.utilization` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricSystemCpuUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricSystemCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricSystemCpuUtilization, descrMetricSystemCpuUtilization,
                                 unitMetricSystemCpuUtilization);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricSystemCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricSystemCpuUtilization, descrMetricSystemCpuUtilization,
                                  unitMetricSystemCpuUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(
      kMetricSystemCpuUtilization, descrMetricSystemCpuUtilization, unitMetricSystemCpuUtilization);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricSystemCpuUtilization, descrMetricSystemCpuUtilization, unitMetricSystemCpuUtilization);
}

/**
 * counter
 */
static constexpr const char *kMetricSystemDiskIo     = "system.disk.io";
static constexpr const char *descrMetricSystemDiskIo = "";
static constexpr const char *unitMetricSystemDiskIo  = "By";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricSystemDiskIo(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemDiskIo, descrMetricSystemDiskIo,
                                    unitMetricSystemDiskIo);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricSystemDiskIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemDiskIo, descrMetricSystemDiskIo,
                                    unitMetricSystemDiskIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricSystemDiskIo(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricSystemDiskIo, descrMetricSystemDiskIo,
                                             unitMetricSystemDiskIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricSystemDiskIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricSystemDiskIo, descrMetricSystemDiskIo,
                                              unitMetricSystemDiskIo);
}

/**
 * Time disk spent activated
 * <p>
 * The real elapsed time ("wall clock") used in the I/O path (time from operations running in
 * parallel are not counted). Measured as: <ul> <li>Linux: Field 13 from <a
 * href="https://www.kernel.org/doc/Documentation/ABI/testing/procfs-diskstats">procfs-diskstats</a></li>
 *   <li>Windows: The complement of
 * <a
 * href="https://learn.microsoft.com/archive/blogs/askcore/windows-performance-monitor-disk-counters-explained#windows-performance-monitor-disk-counters-explained">"Disk%
 * Idle Time"</a> performance counter: @code uptime * (100 - "Disk\% Idle Time") / 100 @endcode</li>
 * </ul>
 * <p>
 * counter
 */
static constexpr const char *kMetricSystemDiskIoTime     = "system.disk.io_time";
static constexpr const char *descrMetricSystemDiskIoTime = "Time disk spent activated";
static constexpr const char *unitMetricSystemDiskIoTime  = "s";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricSystemDiskIoTime(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemDiskIoTime, descrMetricSystemDiskIoTime,
                                    unitMetricSystemDiskIoTime);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricSystemDiskIoTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemDiskIoTime, descrMetricSystemDiskIoTime,
                                    unitMetricSystemDiskIoTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemDiskIoTime(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricSystemDiskIoTime, descrMetricSystemDiskIoTime,
                                             unitMetricSystemDiskIoTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemDiskIoTime(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricSystemDiskIoTime, descrMetricSystemDiskIoTime,
                                              unitMetricSystemDiskIoTime);
}

/**
 * The total storage capacity of the disk
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricSystemDiskLimit     = "system.disk.limit";
static constexpr const char *descrMetricSystemDiskLimit = "The total storage capacity of the disk";
static constexpr const char *unitMetricSystemDiskLimit  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemDiskLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemDiskLimit, descrMetricSystemDiskLimit,
                                         unitMetricSystemDiskLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemDiskLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemDiskLimit, descrMetricSystemDiskLimit,
                                          unitMetricSystemDiskLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemDiskLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricSystemDiskLimit, descrMetricSystemDiskLimit, unitMetricSystemDiskLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemDiskLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricSystemDiskLimit, descrMetricSystemDiskLimit, unitMetricSystemDiskLimit);
}

/**
 * counter
 */
static constexpr const char *kMetricSystemDiskMerged     = "system.disk.merged";
static constexpr const char *descrMetricSystemDiskMerged = "";
static constexpr const char *unitMetricSystemDiskMerged  = "{operation}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricSystemDiskMerged(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemDiskMerged, descrMetricSystemDiskMerged,
                                    unitMetricSystemDiskMerged);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricSystemDiskMerged(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemDiskMerged, descrMetricSystemDiskMerged,
                                    unitMetricSystemDiskMerged);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemDiskMerged(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricSystemDiskMerged, descrMetricSystemDiskMerged,
                                             unitMetricSystemDiskMerged);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemDiskMerged(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricSystemDiskMerged, descrMetricSystemDiskMerged,
                                              unitMetricSystemDiskMerged);
}

/**
 * Sum of the time each operation took to complete
 * <p>
 * Because it is the sum of time each request took, parallel-issued requests each contribute to make
 * the count grow. Measured as: <ul> <li>Linux: Fields 7 & 11 from <a
 * href="https://www.kernel.org/doc/Documentation/ABI/testing/procfs-diskstats">procfs-diskstats</a></li>
 *   <li>Windows: "Avg. Disk sec/Read" perf counter multiplied by "Disk Reads/sec" perf counter
 * (similar for Writes)</li>
 * </ul>
 * <p>
 * counter
 */
static constexpr const char *kMetricSystemDiskOperationTime = "system.disk.operation_time";
static constexpr const char *descrMetricSystemDiskOperationTime =
    "Sum of the time each operation took to complete";
static constexpr const char *unitMetricSystemDiskOperationTime = "s";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemDiskOperationTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemDiskOperationTime,
                                    descrMetricSystemDiskOperationTime,
                                    unitMetricSystemDiskOperationTime);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricSystemDiskOperationTime(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemDiskOperationTime,
                                    descrMetricSystemDiskOperationTime,
                                    unitMetricSystemDiskOperationTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemDiskOperationTime(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricSystemDiskOperationTime,
                                             descrMetricSystemDiskOperationTime,
                                             unitMetricSystemDiskOperationTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemDiskOperationTime(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricSystemDiskOperationTime,
                                              descrMetricSystemDiskOperationTime,
                                              unitMetricSystemDiskOperationTime);
}

/**
 * counter
 */
static constexpr const char *kMetricSystemDiskOperations     = "system.disk.operations";
static constexpr const char *descrMetricSystemDiskOperations = "";
static constexpr const char *unitMetricSystemDiskOperations  = "{operation}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemDiskOperations(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemDiskOperations, descrMetricSystemDiskOperations,
                                    unitMetricSystemDiskOperations);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricSystemDiskOperations(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemDiskOperations, descrMetricSystemDiskOperations,
                                    unitMetricSystemDiskOperations);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemDiskOperations(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricSystemDiskOperations, descrMetricSystemDiskOperations, unitMetricSystemDiskOperations);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemDiskOperations(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricSystemDiskOperations, descrMetricSystemDiskOperations, unitMetricSystemDiskOperations);
}

/**
 * The total storage capacity of the filesystem
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricSystemFilesystemLimit = "system.filesystem.limit";
static constexpr const char *descrMetricSystemFilesystemLimit =
    "The total storage capacity of the filesystem";
static constexpr const char *unitMetricSystemFilesystemLimit = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemFilesystemLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemFilesystemLimit,
                                         descrMetricSystemFilesystemLimit,
                                         unitMetricSystemFilesystemLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemFilesystemLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemFilesystemLimit,
                                          descrMetricSystemFilesystemLimit,
                                          unitMetricSystemFilesystemLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemFilesystemLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricSystemFilesystemLimit,
                                                   descrMetricSystemFilesystemLimit,
                                                   unitMetricSystemFilesystemLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemFilesystemLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricSystemFilesystemLimit,
                                                    descrMetricSystemFilesystemLimit,
                                                    unitMetricSystemFilesystemLimit);
}

/**
 * Reports a filesystem's space usage across different states.
 * <p>
 * The sum of all @code system.filesystem.usage @endcode values over the different @code
 * system.filesystem.state @endcode attributes SHOULD equal the total storage capacity of the
 * filesystem, that is @code system.filesystem.limit @endcode. <p> updowncounter
 */
static constexpr const char *kMetricSystemFilesystemUsage = "system.filesystem.usage";
static constexpr const char *descrMetricSystemFilesystemUsage =
    "Reports a filesystem's space usage across different states.";
static constexpr const char *unitMetricSystemFilesystemUsage = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemFilesystemUsage,
                                         descrMetricSystemFilesystemUsage,
                                         unitMetricSystemFilesystemUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemFilesystemUsage,
                                          descrMetricSystemFilesystemUsage,
                                          unitMetricSystemFilesystemUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricSystemFilesystemUsage,
                                                   descrMetricSystemFilesystemUsage,
                                                   unitMetricSystemFilesystemUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricSystemFilesystemUsage,
                                                    descrMetricSystemFilesystemUsage,
                                                    unitMetricSystemFilesystemUsage);
}

/**
 * gauge
 */
static constexpr const char *kMetricSystemFilesystemUtilization = "system.filesystem.utilization";
static constexpr const char *descrMetricSystemFilesystemUtilization = "";
static constexpr const char *unitMetricSystemFilesystemUtilization  = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricSystemFilesystemUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricSystemFilesystemUtilization,
                                 descrMetricSystemFilesystemUtilization,
                                 unitMetricSystemFilesystemUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricSystemFilesystemUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricSystemFilesystemUtilization,
                                  descrMetricSystemFilesystemUtilization,
                                  unitMetricSystemFilesystemUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemFilesystemUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricSystemFilesystemUtilization,
                                           descrMetricSystemFilesystemUtilization,
                                           unitMetricSystemFilesystemUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemFilesystemUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricSystemFilesystemUtilization,
                                            descrMetricSystemFilesystemUtilization,
                                            unitMetricSystemFilesystemUtilization);
}

/**
 * An estimate of how much memory is available for starting new applications, without causing
 * swapping <p> This is an alternative to @code system.memory.usage @endcode metric with @code
 * state=free @endcode. Linux starting from 3.14 exports "available" memory. It takes "free" memory
 * as a baseline, and then factors in kernel-specific values. This is supposed to be more accurate
 * than just "free" memory. For reference, see the calculations <a
 * href="https://superuser.com/a/980821">here</a>. See also @code MemAvailable @endcode in <a
 * href="https://man7.org/linux/man-pages/man5/proc.5.html">/proc/meminfo</a>. <p> updowncounter
 */
static constexpr const char *kMetricSystemLinuxMemoryAvailable = "system.linux.memory.available";
static constexpr const char *descrMetricSystemLinuxMemoryAvailable =
    "An estimate of how much memory is available for starting new applications, without causing "
    "swapping";
static constexpr const char *unitMetricSystemLinuxMemoryAvailable = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemLinuxMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemLinuxMemoryAvailable,
                                         descrMetricSystemLinuxMemoryAvailable,
                                         unitMetricSystemLinuxMemoryAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemLinuxMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemLinuxMemoryAvailable,
                                          descrMetricSystemLinuxMemoryAvailable,
                                          unitMetricSystemLinuxMemoryAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemLinuxMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricSystemLinuxMemoryAvailable,
                                                   descrMetricSystemLinuxMemoryAvailable,
                                                   unitMetricSystemLinuxMemoryAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemLinuxMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricSystemLinuxMemoryAvailable,
                                                    descrMetricSystemLinuxMemoryAvailable,
                                                    unitMetricSystemLinuxMemoryAvailable);
}

/**
 * Reports the memory used by the Linux kernel for managing caches of frequently used objects.
 * <p>
 * The sum over the @code reclaimable @endcode and @code unreclaimable @endcode state values in
 * @code linux.memory.slab.usage @endcode SHOULD be equal to the total slab memory available on the
 * system. Note that the total slab memory is not constant and may vary over time. See also the <a
 * href="https://blogs.oracle.com/linux/post/understanding-linux-kernel-memory-statistics">Slab
 * allocator</a> and @code Slab @endcode in <a
 * href="https://man7.org/linux/man-pages/man5/proc.5.html">/proc/meminfo</a>. <p> updowncounter
 */
static constexpr const char *kMetricSystemLinuxMemorySlabUsage = "system.linux.memory.slab.usage";
static constexpr const char *descrMetricSystemLinuxMemorySlabUsage =
    "Reports the memory used by the Linux kernel for managing caches of frequently used objects.";
static constexpr const char *unitMetricSystemLinuxMemorySlabUsage = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemLinuxMemorySlabUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemLinuxMemorySlabUsage,
                                         descrMetricSystemLinuxMemorySlabUsage,
                                         unitMetricSystemLinuxMemorySlabUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemLinuxMemorySlabUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemLinuxMemorySlabUsage,
                                          descrMetricSystemLinuxMemorySlabUsage,
                                          unitMetricSystemLinuxMemorySlabUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemLinuxMemorySlabUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricSystemLinuxMemorySlabUsage,
                                                   descrMetricSystemLinuxMemorySlabUsage,
                                                   unitMetricSystemLinuxMemorySlabUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemLinuxMemorySlabUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricSystemLinuxMemorySlabUsage,
                                                    descrMetricSystemLinuxMemorySlabUsage,
                                                    unitMetricSystemLinuxMemorySlabUsage);
}

/**
 * Total memory available in the system.
 * <p>
 * Its value SHOULD equal the sum of @code system.memory.state @endcode over all states.
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricSystemMemoryLimit     = "system.memory.limit";
static constexpr const char *descrMetricSystemMemoryLimit = "Total memory available in the system.";
static constexpr const char *unitMetricSystemMemoryLimit  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemMemoryLimit, descrMetricSystemMemoryLimit,
                                         unitMetricSystemMemoryLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemMemoryLimit, descrMetricSystemMemoryLimit,
                                          unitMetricSystemMemoryLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricSystemMemoryLimit, descrMetricSystemMemoryLimit, unitMetricSystemMemoryLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricSystemMemoryLimit, descrMetricSystemMemoryLimit, unitMetricSystemMemoryLimit);
}

/**
 * Shared memory used (mostly by tmpfs).
 * <p>
 * Equivalent of @code shared @endcode from <a
 * href="https://man7.org/linux/man-pages/man1/free.1.html">@code free @endcode command</a> or
 * @code Shmem @endcode from <a href="https://man7.org/linux/man-pages/man5/proc.5.html">@code
 * /proc/meminfo @endcode</a>" <p> updowncounter
 */
static constexpr const char *kMetricSystemMemoryShared = "system.memory.shared";
static constexpr const char *descrMetricSystemMemoryShared =
    "Shared memory used (mostly by tmpfs).";
static constexpr const char *unitMetricSystemMemoryShared = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemMemoryShared(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemMemoryShared, descrMetricSystemMemoryShared,
                                         unitMetricSystemMemoryShared);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemMemoryShared(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemMemoryShared, descrMetricSystemMemoryShared,
                                          unitMetricSystemMemoryShared);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemMemoryShared(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricSystemMemoryShared, descrMetricSystemMemoryShared, unitMetricSystemMemoryShared);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemMemoryShared(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricSystemMemoryShared, descrMetricSystemMemoryShared, unitMetricSystemMemoryShared);
}

/**
 * Reports memory in use by state.
 * <p>
 * The sum over all @code system.memory.state @endcode values SHOULD equal the total memory
 * available on the system, that is @code system.memory.limit @endcode.
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricSystemMemoryUsage     = "system.memory.usage";
static constexpr const char *descrMetricSystemMemoryUsage = "Reports memory in use by state.";
static constexpr const char *unitMetricSystemMemoryUsage  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemMemoryUsage, descrMetricSystemMemoryUsage,
                                         unitMetricSystemMemoryUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemMemoryUsage, descrMetricSystemMemoryUsage,
                                          unitMetricSystemMemoryUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricSystemMemoryUsage, descrMetricSystemMemoryUsage, unitMetricSystemMemoryUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricSystemMemoryUsage, descrMetricSystemMemoryUsage, unitMetricSystemMemoryUsage);
}

/**
 * gauge
 */
static constexpr const char *kMetricSystemMemoryUtilization     = "system.memory.utilization";
static constexpr const char *descrMetricSystemMemoryUtilization = "";
static constexpr const char *unitMetricSystemMemoryUtilization  = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricSystemMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricSystemMemoryUtilization, descrMetricSystemMemoryUtilization,
                                 unitMetricSystemMemoryUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricSystemMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricSystemMemoryUtilization,
                                  descrMetricSystemMemoryUtilization,
                                  unitMetricSystemMemoryUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricSystemMemoryUtilization,
                                           descrMetricSystemMemoryUtilization,
                                           unitMetricSystemMemoryUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricSystemMemoryUtilization,
                                            descrMetricSystemMemoryUtilization,
                                            unitMetricSystemMemoryUtilization);
}

/**
 * updowncounter
 */
static constexpr const char *kMetricSystemNetworkConnections     = "system.network.connections";
static constexpr const char *descrMetricSystemNetworkConnections = "";
static constexpr const char *unitMetricSystemNetworkConnections  = "{connection}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemNetworkConnections(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemNetworkConnections,
                                         descrMetricSystemNetworkConnections,
                                         unitMetricSystemNetworkConnections);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemNetworkConnections(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemNetworkConnections,
                                          descrMetricSystemNetworkConnections,
                                          unitMetricSystemNetworkConnections);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemNetworkConnections(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricSystemNetworkConnections,
                                                   descrMetricSystemNetworkConnections,
                                                   unitMetricSystemNetworkConnections);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemNetworkConnections(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricSystemNetworkConnections,
                                                    descrMetricSystemNetworkConnections,
                                                    unitMetricSystemNetworkConnections);
}

/**
 * Count of packets that are dropped or discarded even though there was no error
 * <p>
 * Measured as:
 * <ul>
 *   <li>Linux: the @code drop @endcode column in @code /proc/dev/net @endcode (<a
 * href="https://web.archive.org/web/20180321091318/http://www.onlamp.com/pub/a/linux/2000/11/16/LinuxAdmin.html">source</a>)</li>
 *   <li>Windows: <a
 * href="https://docs.microsoft.com/windows/win32/api/netioapi/ns-netioapi-mib_if_row2">@code
 * InDiscards @endcode/@code OutDiscards @endcode</a> from <a
 * href="https://docs.microsoft.com/windows/win32/api/netioapi/nf-netioapi-getifentry2">@code
 * GetIfEntry2 @endcode</a></li>
 * </ul>
 * <p>
 * counter
 */
static constexpr const char *kMetricSystemNetworkDropped = "system.network.dropped";
static constexpr const char *descrMetricSystemNetworkDropped =
    "Count of packets that are dropped or discarded even though there was no error";
static constexpr const char *unitMetricSystemNetworkDropped = "{packet}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemNetworkDropped(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemNetworkDropped, descrMetricSystemNetworkDropped,
                                    unitMetricSystemNetworkDropped);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricSystemNetworkDropped(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemNetworkDropped, descrMetricSystemNetworkDropped,
                                    unitMetricSystemNetworkDropped);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemNetworkDropped(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricSystemNetworkDropped, descrMetricSystemNetworkDropped, unitMetricSystemNetworkDropped);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemNetworkDropped(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricSystemNetworkDropped, descrMetricSystemNetworkDropped, unitMetricSystemNetworkDropped);
}

/**
 * Count of network errors detected
 * <p>
 * Measured as:
 * <ul>
 *   <li>Linux: the @code errs @endcode column in @code /proc/dev/net @endcode (<a
 * href="https://web.archive.org/web/20180321091318/http://www.onlamp.com/pub/a/linux/2000/11/16/LinuxAdmin.html">source</a>).</li>
 *   <li>Windows: <a
 * href="https://docs.microsoft.com/windows/win32/api/netioapi/ns-netioapi-mib_if_row2">@code
 * InErrors @endcode/@code OutErrors @endcode</a> from <a
 * href="https://docs.microsoft.com/windows/win32/api/netioapi/nf-netioapi-getifentry2">@code
 * GetIfEntry2 @endcode</a>.</li>
 * </ul>
 * <p>
 * counter
 */
static constexpr const char *kMetricSystemNetworkErrors     = "system.network.errors";
static constexpr const char *descrMetricSystemNetworkErrors = "Count of network errors detected";
static constexpr const char *unitMetricSystemNetworkErrors  = "{error}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemNetworkErrors, descrMetricSystemNetworkErrors,
                                    unitMetricSystemNetworkErrors);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricSystemNetworkErrors(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemNetworkErrors, descrMetricSystemNetworkErrors,
                                    unitMetricSystemNetworkErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricSystemNetworkErrors, descrMetricSystemNetworkErrors, unitMetricSystemNetworkErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricSystemNetworkErrors, descrMetricSystemNetworkErrors, unitMetricSystemNetworkErrors);
}

/**
 * counter
 */
static constexpr const char *kMetricSystemNetworkIo     = "system.network.io";
static constexpr const char *descrMetricSystemNetworkIo = "";
static constexpr const char *unitMetricSystemNetworkIo  = "By";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricSystemNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemNetworkIo, descrMetricSystemNetworkIo,
                                    unitMetricSystemNetworkIo);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricSystemNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemNetworkIo, descrMetricSystemNetworkIo,
                                    unitMetricSystemNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemNetworkIo(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricSystemNetworkIo, descrMetricSystemNetworkIo,
                                             unitMetricSystemNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemNetworkIo(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricSystemNetworkIo, descrMetricSystemNetworkIo,
                                              unitMetricSystemNetworkIo);
}

/**
 * counter
 */
static constexpr const char *kMetricSystemNetworkPackets     = "system.network.packets";
static constexpr const char *descrMetricSystemNetworkPackets = "";
static constexpr const char *unitMetricSystemNetworkPackets  = "{packet}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemNetworkPackets(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemNetworkPackets, descrMetricSystemNetworkPackets,
                                    unitMetricSystemNetworkPackets);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricSystemNetworkPackets(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemNetworkPackets, descrMetricSystemNetworkPackets,
                                    unitMetricSystemNetworkPackets);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemNetworkPackets(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricSystemNetworkPackets, descrMetricSystemNetworkPackets, unitMetricSystemNetworkPackets);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemNetworkPackets(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricSystemNetworkPackets, descrMetricSystemNetworkPackets, unitMetricSystemNetworkPackets);
}

/**
 * counter
 */
static constexpr const char *kMetricSystemPagingFaults     = "system.paging.faults";
static constexpr const char *descrMetricSystemPagingFaults = "";
static constexpr const char *unitMetricSystemPagingFaults  = "{fault}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricSystemPagingFaults(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemPagingFaults, descrMetricSystemPagingFaults,
                                    unitMetricSystemPagingFaults);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricSystemPagingFaults(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemPagingFaults, descrMetricSystemPagingFaults,
                                    unitMetricSystemPagingFaults);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemPagingFaults(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricSystemPagingFaults, descrMetricSystemPagingFaults, unitMetricSystemPagingFaults);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemPagingFaults(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricSystemPagingFaults, descrMetricSystemPagingFaults, unitMetricSystemPagingFaults);
}

/**
 * counter
 */
static constexpr const char *kMetricSystemPagingOperations     = "system.paging.operations";
static constexpr const char *descrMetricSystemPagingOperations = "";
static constexpr const char *unitMetricSystemPagingOperations  = "{operation}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemPagingOperations(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemPagingOperations,
                                    descrMetricSystemPagingOperations,
                                    unitMetricSystemPagingOperations);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricSystemPagingOperations(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemPagingOperations,
                                    descrMetricSystemPagingOperations,
                                    unitMetricSystemPagingOperations);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemPagingOperations(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricSystemPagingOperations,
                                             descrMetricSystemPagingOperations,
                                             unitMetricSystemPagingOperations);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemPagingOperations(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricSystemPagingOperations,
                                              descrMetricSystemPagingOperations,
                                              unitMetricSystemPagingOperations);
}

/**
 * Unix swap or windows pagefile usage
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricSystemPagingUsage     = "system.paging.usage";
static constexpr const char *descrMetricSystemPagingUsage = "Unix swap or windows pagefile usage";
static constexpr const char *unitMetricSystemPagingUsage  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemPagingUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemPagingUsage, descrMetricSystemPagingUsage,
                                         unitMetricSystemPagingUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemPagingUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemPagingUsage, descrMetricSystemPagingUsage,
                                          unitMetricSystemPagingUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemPagingUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricSystemPagingUsage, descrMetricSystemPagingUsage, unitMetricSystemPagingUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemPagingUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricSystemPagingUsage, descrMetricSystemPagingUsage, unitMetricSystemPagingUsage);
}

/**
 * gauge
 */
static constexpr const char *kMetricSystemPagingUtilization     = "system.paging.utilization";
static constexpr const char *descrMetricSystemPagingUtilization = "";
static constexpr const char *unitMetricSystemPagingUtilization  = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricSystemPagingUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricSystemPagingUtilization, descrMetricSystemPagingUtilization,
                                 unitMetricSystemPagingUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricSystemPagingUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricSystemPagingUtilization,
                                  descrMetricSystemPagingUtilization,
                                  unitMetricSystemPagingUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemPagingUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricSystemPagingUtilization,
                                           descrMetricSystemPagingUtilization,
                                           unitMetricSystemPagingUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemPagingUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricSystemPagingUtilization,
                                            descrMetricSystemPagingUtilization,
                                            unitMetricSystemPagingUtilization);
}

/**
 * Total number of processes in each state
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricSystemProcessCount = "system.process.count";
static constexpr const char *descrMetricSystemProcessCount =
    "Total number of processes in each state";
static constexpr const char *unitMetricSystemProcessCount = "{process}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricSystemProcessCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricSystemProcessCount, descrMetricSystemProcessCount,
                                         unitMetricSystemProcessCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricSystemProcessCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricSystemProcessCount, descrMetricSystemProcessCount,
                                          unitMetricSystemProcessCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemProcessCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricSystemProcessCount, descrMetricSystemProcessCount, unitMetricSystemProcessCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemProcessCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricSystemProcessCount, descrMetricSystemProcessCount, unitMetricSystemProcessCount);
}

/**
 * Total number of processes created over uptime of the host
 * <p>
 * counter
 */
static constexpr const char *kMetricSystemProcessCreated = "system.process.created";
static constexpr const char *descrMetricSystemProcessCreated =
    "Total number of processes created over uptime of the host";
static constexpr const char *unitMetricSystemProcessCreated = "{process}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricSystemProcessCreated(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricSystemProcessCreated, descrMetricSystemProcessCreated,
                                    unitMetricSystemProcessCreated);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricSystemProcessCreated(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricSystemProcessCreated, descrMetricSystemProcessCreated,
                                    unitMetricSystemProcessCreated);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricSystemProcessCreated(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricSystemProcessCreated, descrMetricSystemProcessCreated, unitMetricSystemProcessCreated);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricSystemProcessCreated(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricSystemProcessCreated, descrMetricSystemProcessCreated, unitMetricSystemProcessCreated);
}

/**
 * The time the system has been running
 * <p>
 * Instrumentations SHOULD use a gauge with type @code double @endcode and measure uptime in seconds
 * as a floating point number with the highest precision available. The actual accuracy would depend
 * on the instrumentation and operating system. <p> gauge
 */
static constexpr const char *kMetricSystemUptime     = "system.uptime";
static constexpr const char *descrMetricSystemUptime = "The time the system has been running";
static constexpr const char *unitMetricSystemUptime  = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricSystemUptime(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricSystemUptime, descrMetricSystemUptime,
                                 unitMetricSystemUptime);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricSystemUptime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricSystemUptime, descrMetricSystemUptime,
                                  unitMetricSystemUptime);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricSystemUptime(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricSystemUptime, descrMetricSystemUptime,
                                           unitMetricSystemUptime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricSystemUptime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricSystemUptime, descrMetricSystemUptime,
                                            unitMetricSystemUptime);
}

}  // namespace system
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
