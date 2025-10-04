/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/transport/grpc/grpc_session_manager.h"

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/transport_layer_manager.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {
namespace {
void appendI64(BSONObjBuilder* b, StringData n, auto v) {
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

std::string GRPCSessionManager::getClientThreadName(const Session& session) const {
    const auto* s = checked_cast<const IngressSession*>(&session);
    if (auto id = s->getRemoteClientId()) {
        return fmt::format("grpc-{}-{}", id->toString(), session.id());
    } else {
        return fmt::format("grpc-{}", session.id());
    }
}

void GRPCSessionManager::configureServiceExecutorContext(mongo::Client* client,
                                                         bool isPrivilegedSession) const {
    auto seCtx = std::make_unique<ServiceExecutorContext>();
    seCtx->setThreadModel(seCtx->kInline);
    stdx::lock_guard lk(*client);
    ServiceExecutorContext::set(client, std::move(seCtx));
}

void GRPCSessionManager::appendStats(BSONObjBuilder* bob) const {
    {
        BSONObjBuilder streams(bob->subobjStart("streams"_sd));

        const auto current = numOpenSessions();
        appendI64(&streams, "current"_sd, current);
        appendI64(&streams, "available"_sd, maxOpenSessions() - current);
        appendI64(&streams, "rejected"_sd, numRejectedSessions());
        appendI64(&streams, "total"_sd, numCreatedSessions());
        appendI64(&streams, "successful"_sd, _successfulSessions.load());

        helloMetrics.serialize(&streams);

        streams.doneFast();
    }

    {
        const auto totalOps = getTotalOperations();
        const auto completedOps = getCompletedOperations();
        BSONObjBuilder ops(bob->subobjStart("operations"_sd));
        appendI64(&ops, "active"_sd, totalOps - completedOps);
        appendI64(&ops, "total"_sd, totalOps);
        ops.doneFast();
    }

    appendI64(bob, "uniqueClientsSeen"_sd, _clientCache->getUniqueClientsSeen());
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
