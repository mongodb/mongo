// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/grpc_session_manager.h"

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/message_filter_hooks.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/transport_layer_manager.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {
using namespace std::literals::string_view_literals;
namespace {
void appendI64(BSONObjBuilder* b, std::string_view n, auto v) {
    b->append(n, static_cast<std::int64_t>(v));
}

GRPCSessionManager* getSessionManager(Client* client) {
    if (!client || !client->session())
        return nullptr;

    auto* tl = client->session()->getTransportLayer();
    if (!tl || !tl->getSessionManager())
        return nullptr;

    return dynamic_cast<GRPCSessionManager*>(tl->getSessionManager());
}

}  // namespace

void GRPCSessionManager::startSession(std::shared_ptr<Session> session) {
    invariant(session);
    MessageHooks::onConnectionEstablished(*session);
    SessionManagerCommon::startSession(std::move(session));
}

std::string GRPCSessionManager::getClientThreadName(const Session& session) const {
    const auto* s = checked_cast<const IngressSession*>(&session);
    if (auto id = s->getRemoteClientId()) {
        return fmt::format("grpc-{}-{}", id->toString(), session.id());
    } else {
        return fmt::format("grpc-{}", session.id());
    }
}

void GRPCSessionManager::configureServiceExecutorContext(mongo::Client& client,
                                                         bool isPrivilegedSession) const {
    auto seCtx = std::make_unique<ServiceExecutorContext>();
    seCtx->setThreadModel(seCtx->kInline);
    std::lock_guard lk(client);
    ServiceExecutorContext::set(&client, std::move(seCtx));
}

void GRPCSessionManager::appendStats(BSONObjBuilder* bob) const {
    {
        BSONObjBuilder streams(bob->subobjStart("streams"sv));

        const auto current = numOpenSessions();
        appendI64(&streams, "current"sv, current);
        appendI64(&streams, "available"sv, maxOpenSessions() - current);
        appendI64(&streams, "rejected"sv, numRejectedSessions());
        appendI64(&streams, "total"sv, numCreatedSessions());
        appendI64(&streams, "successful"sv, _successfulSessions.load());

        helloMetrics.serialize(&streams);

        streams.doneFast();
    }

    {
        const auto totalOps = getTotalOperations();
        const auto completedOps = getCompletedOperations();
        BSONObjBuilder ops(bob->subobjStart("operations"sv));
        appendI64(&ops, "active"sv, totalOps - completedOps);
        appendI64(&ops, "total"sv, totalOps);
        ops.doneFast();
    }

    appendI64(bob, "uniqueClientsSeen"sv, _clientCache->getUniqueClientsSeen());
}

void GRPCSessionManager::endSessionByClient(mongo::Client* client) {
    SessionManagerCommon::endSessionByClient(client);

    auto session = dynamic_cast<IngressSession*>(client->session().get());
    massert(8076902,
            "GRPCSessionManager::endSessionByClient handling non grpc::IngressSession instance",
            session);

    auto optStatus = session->terminationStatus();
    invariant(optStatus);
    if (optStatus->isOK())
        _successfulSessions.fetchAndAddRelaxed(1);
}

}  // namespace mongo::transport::grpc
