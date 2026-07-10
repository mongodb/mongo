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

#include "mongo/bson/bsonobj.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

class ServiceContext;

/**
 * Owns the OpenTelemetry instruments for disk I/O metrics. The set of devices is fixed at
 * construction time: disks that appear after startup are not tracked.
 */
class DiskMetrics {
public:
    /**
     * Registers per-device counters for each of the provided disk devices.
     */
    explicit DiskMetrics(std::vector<std::string> disks);
    ~DiskMetrics();

    /**
     * Walks the BSON and adds deltas to the registered counters. Devices not declared at
     * construction time are ignored.
     */
    void update(BSONObj disksBson);

private:
    class Impl;
    static otel::metrics::DynamicMetricNameMaker::Passkey dyn_metric_passkey() {
        return {};
    }
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry disk I/O counters and starts a periodic job that samples
 * once per second. No-op on unsupported platforms.
 */
[[MONGO_MOD_PUBLIC]] void installDiskOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
