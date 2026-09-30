// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/modules.h"

#include <memory>
#include <unordered_map>

namespace mongo {

/**
 * Owns the OpenTelemetry instruments for metrics gathered from replication sessions. These include
 * congestion window size and receive queue size.
 */
class SessionMetrics {
public:
    SessionMetrics();
    ~SessionMetrics();

    void update(const std::vector<transport::SessionStats>& info);
    void recordCollectError();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry replication session info instruments and starts a periodic job (1 Hz)
 * that reads stats from AsioTransportLayer for each replication socket. No-op on unsupported
 * platforms.
 */
[[MONGO_MOD_PUBLIC]] void installReplicationSessionOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
