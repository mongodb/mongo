// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/client_transport_observer.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/util/modules.h"

namespace mongo {

class ServiceEntryPointCryptD final : public ServiceEntryPoint {
public:
    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request,
                                     Date_t started) noexcept final;
};

class ClientObserverCryptD final : public transport::ClientTransportObserver {
public:
    void onClientConnect(Client*) final;
};

}  // namespace mongo
