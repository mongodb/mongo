// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/service_context.h"
#include "mongo/transport/backpressure_connection_metrics.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/transport_layer_manager.h"

namespace mongo {

BackpressureConnectionMetrics BackpressureConnectionMetrics::collect(ServiceContext* svcCtx) {
    BackpressureConnectionMetrics out;
    auto* tlm = svcCtx->getTransportLayerManager();
    if (!tlm) {
        return out;
    }

    tlm->forEach([&](transport::TransportLayer* tl) {
        if (auto* sm = dynamic_cast<transport::SessionManagerCommon*>(tl->getSessionManager())) {
            if (sm->shouldIncludeInConnectionsServerStatus()) {
                out += sm->backpressureConnectionMetrics;
            }
        }
    });
    return out;
}

}  // namespace mongo
