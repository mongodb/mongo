// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/client_transport_observer.h"
#include "mongo/transport/grpc/client_cache.h"
#include "mongo/transport/session_manager_common.h"

#include <memory>

namespace mongo::transport::grpc {

/**
 * GRPC specialization of SessionManagerCommon.
 */
class GRPCSessionManager : public SessionManagerCommon {
public:
    GRPCSessionManager(ServiceContext* svcCtx,
                       std::shared_ptr<ClientCache> clientCache,
                       std::vector<std::shared_ptr<ClientTransportObserver>> observers = {})
        : SessionManagerCommon(svcCtx, std::move(observers)),
          _clientCache(std::move(clientCache)) {}

    void startSession(std::shared_ptr<Session> session) override;

    void appendStats(BSONObjBuilder* bob) const;
    void endSessionByClient(mongo::Client* client) override;

protected:
    std::string getClientThreadName(const Session&) const override;
    void configureServiceExecutorContext(mongo::Client& client,
                                         bool isPrivilegedSession) const override;

    Atomic<std::size_t> _successfulSessions{0};
    std::shared_ptr<ClientCache> _clientCache;
};

}  // namespace mongo::transport::grpc
