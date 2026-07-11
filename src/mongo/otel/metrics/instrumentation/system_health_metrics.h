// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {

/**
 * Snapshot of system-level health values sampled from /proc/stat and /proc/sys/fs/file-nr.
 *
 * CPU times are cumulative milliseconds since boot. Thread and fd counts are instantaneous.
 */
struct SystemHealthSnapshot {
    int64_t cpuUserMs{0};
    int64_t cpuNiceMs{0};
    int64_t cpuSystemMs{0};
    int64_t cpuIdleMs{0};
    int64_t cpuIowaitMs{0};
    int64_t cpuIrqMs{0};
    int64_t cpuSoftirqMs{0};
    int64_t cpuStealMs{0};
    int64_t cpuGuestMs{0};
    int64_t cpuGuestNiceMs{0};

    // non-CPU health metrics
    int64_t procsRunning{0};
    int64_t procsBlocked{0};
    int64_t fdOpen{0};
};

/**
 * Owns the OpenTelemetry instruments for system health metrics:
 *   - mongodb.system.cpu.time: counter of cumulative CPU time (ms) since boot, read directly from
 *     /proc/stat on each sample, broken down by a `mode` attribute (user, nice, system, idle,
 *     iowait, irq, softirq, steal, guest, guest_nice).
 *   - mongodb.system.cpu.utilization: per-mode CPU utilization, summing to 1.0. If there has been
 *     no activity, the gauges return their most recent value.
 *   - mongodb.system.thread.{active,queued}: gauges for OS-level runnable/blocked process counts.
 *   - mongodb.system.fd.open: gauge for the system-wide open file handle count.
 *   - mongodb.systemHealth.collectErrors: counter of failed snapshot collections.
 *
 * Thread and fd instruments are set to the latest snapshot value on each call to update().
 */

class SystemHealthMetrics {
public:
    SystemHealthMetrics();
    ~SystemHealthMetrics();

    void update(const SystemHealthSnapshot& snap);
    void recordCollectError();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry system health instruments and starts a periodic job (1 Hz) that
 * reads /proc/stat and /proc/sys/fs/file-nr to push the latest values.
 * No-op on unsupported platforms.
 */
[[MONGO_MOD_PUBLIC]] void installSystemHealthOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
