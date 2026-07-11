// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * Owns the OpenTelemetry instruments that track ingress connection state
 * (current, available, totalCreated, rejected, active), matching the fields
 * reported by serverStatus.connections. Each call to `update` sets the latest
 * values from a ConnectionsStatsSnapshot.
 */
class ConnectionsMetrics {
public:
    ConnectionsMetrics();
    ~ConnectionsMetrics();

    void update(const transport::ConnectionsStatsSnapshot& snap);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry gauges for ingress connection state and starts a
 * periodic job (1 Hz) that samples the AsioSessionManager and pushes the
 * latest values. Intended to be called once at startup from mongod_main.
 */
[[MONGO_MOD_PUBLIC]] void installConnectionsOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
