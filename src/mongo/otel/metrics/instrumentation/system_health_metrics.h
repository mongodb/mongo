/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
MONGO_MOD_PUBLIC void installSystemHealthOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
