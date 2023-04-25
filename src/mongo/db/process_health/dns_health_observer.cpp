/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/process_health/dns_health_observer.h"

#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/util/dns_name.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/hostname_canonicalization.h"
#include <algorithm>
#include <random>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


namespace mongo {
namespace process_health {

MONGO_FAIL_POINT_DEFINE(dnsHealthObserverFp);

Future<HealthCheckStatus> DnsHealthObserver::periodicCheckImpl(
    PeriodicHealthCheckContext&& periodicCheckContext) {
    LOGV2_DEBUG(5938401, 2, "DNS health observer executing");


    auto makeFailedHealthCheckFuture = [this](const Status& status) {
        return Future<HealthCheckStatus>::makeReady(
            makeSimpleFailedStatus(Severity::kFailure, {status}));
    };

    ConnectionString connString;
    auto isFailPointActive = false;
    if (MONGO_unlikely(dnsHealthObserverFp.shouldFail())) {
        isFailPointActive = true;
        dnsHealthObserverFp.executeIf(
            [this, &connString](const BSONObj& data) {
                auto fpHostname = data["hostname"].String();
                connString = ConnectionString::forReplicaSet("serverWithBadHostName",
                                                             {HostAndPort(fpHostname, 27017)});
            },
            [&](const BSONObj& data) { return !data.isEmpty(); });
    }

    if (!isFailPointActive) {
        auto client = _svcCtx->makeClient("DNSHealthObserver");

        // TODO(SERVER-74659): Please revisit if this thread could be made killable.
        {
            stdx::lock_guard<Client> lk(*client.get());
            client.get()->setSystemOperationUnkillableByStepdown(lk);
        }

        auto opCtx = client->makeOperationContext();
        auto const shardRegistry = Grid::get(_svcCtx)->shardRegistry();
        auto shardIds = shardRegistry->getAllShardIds(opCtx.get());

        if (shardIds.size() == 0) {
            connString = shardRegistry->getConfigServerConnectionString();
        } else {
            auto shardSW = shardRegistry->getShard(opCtx.get(),
                                                   shardIds.at(_random.nextInt32(shardIds.size())));
            auto shardSWStatus = shardSW.getStatus();
            if (shardSWStatus.isOK()) {
                connString = shardSW.getValue()->getConnString();
            } else {
                return makeFailedHealthCheckFuture(shardSWStatus);
            }
        }
    }

    auto servers = connString.getServers();
    if (servers.empty()) {
        return makeFailedHealthCheckFuture(
            Status(ErrorCodes::NetworkTimeout, "No hostnames for DNS health check"));
    }

    std::shuffle(servers.begin(), servers.end(), _random.urbg());

    auto completionPf = makePromiseFuture<HealthCheckStatus>();

    auto status = periodicCheckContext.taskExecutor->scheduleWork(
        [this, servers, promise = std::move(completionPf.promise)](
            const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
            try {
                auto statusWith =
                    getHostFQDNs(servers.front().host(), HostnameCanonicalizationMode::kForward);
                if (statusWith.isOK() && !statusWith.getValue().empty()) {
                    promise.emplaceValue(makeHealthyStatus());
                } else {
                    promise.emplaceValue(
                        makeSimpleFailedStatus(Severity::kFailure, {statusWith.getStatus()}));
                }
            } catch (const DBException& e) {
                promise.emplaceValue(makeSimpleFailedStatus(Severity::kFailure, {e.toStatus()}));
            }
        });

    return std::move(completionPf.future);
}

namespace {
MONGO_INITIALIZER(DnsHealthObserver)(InitializerContext*) {
    HealthObserverRegistration::registerObserverFactory(
        [](ServiceContext* svcCtx) { return std::make_unique<DnsHealthObserver>(svcCtx); });
}
}  // namespace

}  // namespace process_health
}  // namespace mongo
