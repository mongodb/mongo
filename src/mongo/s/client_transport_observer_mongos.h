// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/transport/client_transport_observer.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Cleans up open cursors and in-progress transactions upon disconnect for clients that
 * are considered bound to the operation state.
 */
class [[MONGO_MOD_PUBLIC]] ClientTransportObserverMongos final
    : public transport::ClientTransportObserver {
    void onClientDisconnect(Client* client) final;
};

}  // namespace mongo
