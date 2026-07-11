// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/dns_health_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/hostname_canonicalization.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

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
        // TODO(SERVER-74659): Please revisit if this thread could be made killable.
        auto client = _svcCtx->getService()->makeClient(
            "DNSHealthObserver", Client::noSession(), ClientOperationKillableByStepdown{false});

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
