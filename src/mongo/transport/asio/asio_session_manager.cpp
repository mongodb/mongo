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

#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/session.h"

namespace mongo::transport {
std::string AsioSessionManager::getClientThreadName(const Session& session) const {
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

    invariant(_svcCtx);
    appendInt("active", _svcCtx->getActiveClientOperations());

    const auto seStats = ServiceExecutorStats::get(_svcCtx);
    appendInt("threaded", seStats.totalClients);
    if (!serverGlobalParams.maxConnsOverride.empty()) {
        appendInt("limitExempt", seStats.limitExempt);
    }

    auto&& hm = HelloMetrics::get(_svcCtx);
    appendInt("exhaustIsMaster", hm->getNumExhaustIsMaster());
    appendInt("exhaustHello", hm->getNumExhaustHello());
    appendInt("awaitingTopologyChanges", hm->getNumAwaitingTopologyChanges());

    if (auto adminExec = ServiceExecutorReserved::get(_svcCtx)) {
        BSONObjBuilder section(bob->subobjStart("adminConnections"));
        adminExec->appendStats(&section);
    }

    SessionManagerCommon::appendStats(bob);
}

}  // namespace mongo::transport
