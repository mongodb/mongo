// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
class BSONObjBuilder;
class Client;

namespace transport {

/**
 * ClientTransportObservers are notified during key events in a Client's lifecycle.
 */
class [[MONGO_MOD_OPEN]] ClientTransportObserver {
public:
    virtual ~ClientTransportObserver() = default;

    /**
     * Called on a new client connection.
     */
    virtual void onClientConnect(Client* client) {}

    /**
     * Called on destruction of a client.
     */
    virtual void onClientDisconnect(Client* client) {}
};

}  // namespace transport
}  // namespace mongo
