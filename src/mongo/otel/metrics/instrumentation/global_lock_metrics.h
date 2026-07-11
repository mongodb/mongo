// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class ServiceContext;
struct GlobalLockStatsSnapshot;

/**
 * Owns the OpenTelemetry instruments that track global-lock state
 * (totalTime, currentQueue.{total,readers,writers}, activeClients.{total,readers,writers}).
 * The gauges are registered at construction; each call to `update` sets the
 * latest values from a GlobalLockStatsSnapshot.
 */
class GlobalLockMetrics {
public:
    GlobalLockMetrics();
    ~GlobalLockMetrics();

    void update(const GlobalLockStatsSnapshot& snap);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry gauges for global-lock state and starts a periodic
 * job (1 Hz) that samples the snapshot utility and pushes the latest values.
 * Intended to be called once at startup from mongod_main.
 */
[[MONGO_MOD_PUBLIC]] void installGlobalLockOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
