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

#include "mongo/transport/asio/asio_session_manager.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_manager.h"

namespace mongo::transport {
namespace {
// "connections" is a legacy name from when only one TransportLayer was in use at any time.
// Asio, being that singular layer inherits the "connection" namespace, while others
// are introduced in their own named section (e.g. "gRPC").
class Connections : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        bool asioSeen = false;
        BSONObjBuilder bb;
        if (auto tlm = opCtx->getServiceContext()->getTransportLayerManager()) {
            tlm->forEach([&](TransportLayer* tl) {
                if (auto sm = dynamic_cast<AsioSessionManager*>(tl->getSessionManager())) {
                    massert(8076900, "Multiple AsioSessionManagers", !asioSeen);
                    asioSeen = true;
                    sm->appendStats(&bb);
                }
            });
        }
        return bb.obj();
    }
};
auto& connections = *ServerStatusSectionBuilder<Connections>("connections");
}  // namespace

std::string AsioSessionManager::getClientThreadName(const Session& session) const {
    using namespace fmt::literals;
    return "conn{}"_format(session.id());
}

void AsioSessionManager::configureServiceExecutorContext(Client* client,
                                                         bool isPrivilegedSession) const {
    // TODO SERVER-77921: use the return value of `Session::isFromRouterPort()` to choose an
    // instance of `ServiceEntryPoint`.
    auto seCtx = std::make_unique<ServiceExecutorContext>();
    seCtx->setThreadModel(ServiceExecutorContext::kSynchronous);
    seCtx->setCanUseReserved(isPrivilegedSession);
    stdx::lock_guard lk(*client);
    ServiceExecutorContext::set(client, std::move(seCtx));
}

void AsioSessionManager::appendStats(BSONObjBuilder* bob) const {
    const auto sessionCount = numOpenSessions();

    const auto appendInt = [&](StringData n, auto v) {
        bob->append(n, static_cast<int>(v));
    };

    appendInt("current", sessionCount);
    appendInt("available", maxOpenSessions() - sessionCount);
    appendInt("totalCreated", numCreatedSessions());
    appendInt("rejected", _rejectedSessions);

    appendInt("active", getActiveOperations());

    // Historically, this number may have differed from "current" since
    // some sessions would have used the non-threaded ServiceExecutorFixed.
    // Currently all sessions are threaded, so this number is redundant.
    appendInt("threaded", sessionCount);
    if (!serverGlobalParams.maxConnsOverride.empty()) {
        appendInt("limitExempt", serviceExecutorStats.limitExempt.load());
    }

    helloMetrics.serialize(bob);

    invariant(_svcCtx);
    if (auto adminExec = ServiceExecutorReserved::get(_svcCtx)) {
        BSONObjBuilder section(bob->subobjStart("adminConnections"));
        adminExec->appendStats(&section);
    }

    bob->append("loadBalanced", _loadBalancedConnections.get());
}

void AsioSessionManager::onClientConnect(Client* client) {
    auto session = client->session();
    if (session && session->isFromLoadBalancer()) {
        _loadBalancedConnections.increment();
    }
}

void AsioSessionManager::onClientDisconnect(Client* client) {
    auto session = client->session();
    if (session && session->isFromLoadBalancer()) {
        _loadBalancedConnections.decrement();
    }
}

}  // namespace mongo::transport
