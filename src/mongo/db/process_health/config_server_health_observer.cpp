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

#include <random>

#include "mongo/db/process_health/health_observer_base.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


namespace mongo {
namespace process_health {

static constexpr Milliseconds kObserverTimeout{30000};
static constexpr Milliseconds kServerRequestTimeout{10000};

/**
 * Implementation of health observer for the Config Server.
 *
 * This observer detects a failure if:
 * 1. We don't have permission to read from the 'shards' collection at the
 *    Config server or
 * 2. We cannot reach any server from a random majority of the servers
 *
 * To avoid an expensive majority read concern read from the primary this observer
 * tries to read only from the 'nearest' server with 'available' read concern.
 * However this check is insufficient. To verify that we have a view on majority
 * it requires at least one response from a random majority.
 */
class ConfigServerHealthObserver final : public HealthObserverBase {
public:
    explicit ConfigServerHealthObserver(ServiceContext* svcCtx);
    ~ConfigServerHealthObserver() = default;

    /**
     * Health observer unique type.
     */
    FaultFacetType getType() const override {
        return FaultFacetType::kConfigServer;
    }

    Milliseconds getObserverTimeout() const override {
        return kObserverTimeout;
    }

    bool isConfigured() const override {
        return true;
    }

    /**
     * Triggers health check.
     * It is guaranteed that the next check is never invoked until the promise for the
     * previous one is filled, thus synchronization can be relaxed.
     */
    Future<HealthCheckStatus> periodicCheckImpl(
        PeriodicHealthCheckContext&& periodicCheckContext) override;

private:
    // Collects the results of one check.
    struct CheckResult {
        // Final criteria that the health check passed.
        bool checkPassed() const {
            // If smoke read check passed and we can reach some servers,
            // as configured by the IDL.
            return smokeReadCheckPassed && requiredCountReachable;
        }

        // The check passed if read from server succeeds.
        bool smokeReadCheckPassed = false;
        bool requiredCountReachable = false;
        std::vector<Status> failures;
    };

    struct CheckContext {
        CheckContext(CancellationToken&& cancellationToken)
            : cancellationToken(std::move(cancellationToken)) {}
        ServiceContext::UniqueClient client;
        ServiceContext::UniqueOperationContext opCtx;
        CancellationToken cancellationToken;
        std::shared_ptr<executor::TaskExecutor> taskExecutor;
        CheckResult result;
    };

    // Implementation of the health check.
    Future<CheckResult> _checkImpl(PeriodicHealthCheckContext&& periodicCheckContext);

    // Try a quick smoke read from 'shards' collection on nearest config server with
    // 'available' read concern.
    // If this succeeds there is no need to read with majority read concern for the
    // reason that if there is an ongoing failover at the Config RS it should not
    // block the health observer.
    void _runSmokeReadShardsCommand(std::shared_ptr<CheckContext> ctx);

    // Returns success only if some replicas can be reached.
    // This is configured by the IDL.
    Future<void> _runPingSeveralReplicas(std::shared_ptr<CheckContext> ctx);

    // Utility method to do a ping.
    // Completion token is different from top level cancelation token, may be
    // canceled when the request is no longer necessary (got enough results).
    Future<void> _runPing(HostAndPort server,
                          std::shared_ptr<CheckContext> ctx,
                          CancellationToken completionToken);

    // Fetches 'serverStatus' from every config server.
    // This check requires all servers to be reachable or the primary to be reachable to pass.
    void _runRequiredCountReachableCheck(std::shared_ptr<CheckContext> ctx, CheckResult* result);
};


ConfigServerHealthObserver::ConfigServerHealthObserver(ServiceContext* svcCtx)
    : HealthObserverBase(svcCtx) {}

Future<HealthCheckStatus> ConfigServerHealthObserver::periodicCheckImpl(
    PeriodicHealthCheckContext&& periodicCheckContext) {
    // The chain is not capturing 'this' for the case the network call outlives the observer.
    return _checkImpl(std::move(periodicCheckContext))
        .then([type = getType()](CheckResult result) mutable -> Future<HealthCheckStatus> {
            if (result.checkPassed()) {
                return HealthObserverBase::makeHealthyStatusWithType(type);
            } else {
                return HealthObserverBase::makeSimpleFailedStatusWithType(
                    type, Severity::kFailure, std::move(result.failures));
            }
        })
        .onError([type = getType()](Status status) -> Future<HealthCheckStatus> {
            std::vector<Status> failures{status};
            return HealthObserverBase::makeSimpleFailedStatusWithType(
                type, Severity::kFailure, std::move(failures));
        });
}

Future<ConfigServerHealthObserver::CheckResult> ConfigServerHealthObserver::_checkImpl(
    PeriodicHealthCheckContext&& periodicCheckContext) {
    auto checkCtx =
        std::make_shared<CheckContext>(std::move(periodicCheckContext.cancellationToken));
    checkCtx->taskExecutor = periodicCheckContext.taskExecutor;
    checkCtx->client = _svcCtx->makeClient("ConfigServerHealthObserver");
    checkCtx->opCtx = checkCtx->client->makeOperationContext();
    checkCtx->opCtx->setDeadlineAfterNowBy(kObserverTimeout, ErrorCodes::ExceededTimeLimit);

    LOGV2_DEBUG(5939001, 3, "Checking Config server health");

    _runSmokeReadShardsCommand(checkCtx);
    if (!checkCtx->result.smokeReadCheckPassed) {
        return checkCtx->result;  // Already failed.
    }

    if (checkCtx->cancellationToken.isCanceled()) {
        // Apparently the server is shutting down, just return
        // incomplete check.
        return checkCtx->result;
    }

    return _runPingSeveralReplicas(checkCtx)
        .then([checkCtx]() mutable {
            // Required config replicas are reachable.
            checkCtx->result.requiredCountReachable = true;
            return checkCtx->result;
        })
        .onError([checkCtx](Status status) mutable {
            checkCtx->result.failures.push_back(status);
            return checkCtx->result;
        });
}

void ConfigServerHealthObserver::_runSmokeReadShardsCommand(std::shared_ptr<CheckContext> ctx) {
    const ReadPreferenceSetting readPref(ReadPreference::Nearest, TagSet{});

    BSONObj readConcernObj = [&] {
        repl::ReadConcernArgs readConcern{repl::ReadConcernLevel::kAvailableReadConcern};
        BSONObjBuilder bob;
        readConcern.appendInfo(&bob);
        return bob.done().getObjectField(repl::ReadConcernArgs::kReadConcernFieldName).getOwned();
    }();

    BSONObjBuilder findCmdBuilder;
    FindCommandRequest findCommand(NamespaceString::kConfigsvrShardsNamespace);
    findCommand.setReadConcern(readConcernObj);
    findCommand.setLimit(1);
    findCommand.setSingleBatch(true);
    findCommand.setMaxTimeMS(Milliseconds(kServerRequestTimeout).count());
    findCommand.serialize(BSONObj(), &findCmdBuilder);

    // `runCommand()` is not futurized so this method is blocking.
    Timer t;
    StatusWith<Shard::CommandResponse> findOneShardResponse{ErrorCodes::HostUnreachable,
                                                            "Config server read was not run"};
    try {
        findOneShardResponse =
            Grid::get(ctx->opCtx.get())
                ->shardRegistry()
                ->getConfigShard()
                ->runCommand(ctx->opCtx.get(),
                             readPref,
                             NamespaceString::kConfigsvrShardsNamespace.db().toString(),
                             findCmdBuilder.done(),
                             kServerRequestTimeout,
                             Shard::RetryPolicy::kNoRetry);
    } catch (const DBException& exc) {
        findOneShardResponse = StatusWith<Shard::CommandResponse>(exc.toStatus());
    }

    if (findOneShardResponse.isOK()) {
        ctx->result.smokeReadCheckPassed = true;
        LOGV2_DEBUG(5939002,
                    3,
                    "Config server smoke check passed",
                    "server"_attr = findOneShardResponse.getValue().hostAndPort,
                    "latency"_attr = t.elapsed());
    } else {
        ctx->result.smokeReadCheckPassed = false;
        LOGV2_DEBUG(5939003,
                    3,
                    "Config server smoke check failed",
                    "status"_attr = findOneShardResponse.getStatus(),
                    "latency"_attr = t.elapsed());
    }
}

Future<void> ConfigServerHealthObserver::_runPingSeveralReplicas(
    std::shared_ptr<CheckContext> ctx) {
    const auto configShard = Grid::get(ctx->opCtx.get())->shardRegistry()->getConfigShard();
    if (!configShard) {
        return Status(ErrorCodes::HostUnreachable, "Config shard not found");
    }
    const auto connectionStr = configShard->getTargeter()->connectionString();

    auto servers = connectionStr.getServers();
    if (servers.empty()) {
        return Status(ErrorCodes::HostUnreachable, "No servers found in Config shard");
    }

    if (servers.size() > static_cast<unsigned>(gConfigReplicasProbed.load())) {
        std::random_device rd;
        std::mt19937 randomGenerator(rd());
        std::shuffle(servers.begin(), servers.end(), randomGenerator);
        // We ping only necessary count of servers.
        servers.resize(gConfigReplicasProbed.load());
    }

    LOGV2_DEBUG(
        5939005, 3, "Health checker starts pinging Config servers", "count"_attr = servers.size());

    // A cancelation source to signal no more work is necessary - either a good server is found
    // or there are no more servers.
    // We also use this cancelation source to block until we can return from this method.
    CancellationSource completionCancellationSource(ctx->cancellationToken);
    // Keeps track of how many servers we reached or failed.
    auto reachedCount = std::make_shared<AtomicWord<int>>();
    auto failedCount = std::make_shared<AtomicWord<int>>();

    for (uint64_t i = 0; i < servers.size(); ++i) {
        const auto& server = servers[i];
        _runPing(server, ctx, completionCancellationSource.token())
            .then([completionCancellationSource, reachedCount]() mutable {
                reachedCount->fetchAndAdd(1);
                if (reachedCount->load() >= gReachableConfigReplicasRequired.load()) {
                    // Sufficient count of servers reached.
                    // This code is racy by design, the cancel() here and below
                    // can be invoked more than once by concurrent threads.
                    completionCancellationSource.cancel();
                }
            })
            .onError([completionCancellationSource, failedCount](Status) {
                // We can be here after the method returns, be careful with memory access.
                failedCount->fetchAndAdd(1);
            })
            .onCompletion([completionCancellationSource,
                           reachedCount,
                           failedCount,
                           total = static_cast<int>(servers.size())](Status) mutable {
                // We can be here after the method returns, be careful with memory access.
                if (reachedCount->load() + failedCount->load() >= total) {
                    invariant(reachedCount->load() + failedCount->load() == total);
                    // We have a result on all servers.
                    completionCancellationSource.cancel();
                }
            })
            .getAsync([](Status) {});
    }

    // Blocks until we reached the conclusion.
    completionCancellationSource.token().onCancel().waitNoThrow().ignore();

    if (reachedCount->load() >= gReachableConfigReplicasRequired.load()) {
        // Sufficient count of servers reached.
        return Future<void>::makeReady(Status::OK());
    }

    LOGV2_DEBUG(5939006,
                3,
                "Health checker was not able to ping the configured count of Config servers",
                "successCount"_attr = reachedCount->load(),
                "failuresCount"_attr = failedCount->load(),
                "serversPinged"_attr = servers.size());
    return Future<void>::makeReady(Status(ErrorCodes::HostUnreachable,
                                          "Config server health checker cannot reach the "
                                          "configured count of replicas"));
}

Future<void> ConfigServerHealthObserver::_runPing(HostAndPort server,
                                                  std::shared_ptr<CheckContext> ctx,
                                                  CancellationToken completionToken) {
    auto completionPf = makePromiseFuture<void>();
    auto promise = std::make_shared<Promise<void>>(std::move(completionPf.promise));
    Timer t;
    ctx->taskExecutor
        ->scheduleRemoteCommand(
            {server, "admin", BSON("ping" << 1), nullptr, kServerRequestTimeout}, completionToken)
        .then([promise, server, t](const executor::RemoteCommandResponse& response) {
            if (!response.status.isOK()) {
                promise->setError(response.status);
            }
            LOGV2_DEBUG(5939007,
                        3,
                        "Health checker was able to ping a Config server replica",
                        "server"_attr = server,
                        "latency"_attr = t.elapsed());
            promise->emplaceValue();
        })
        .onError([promise](Status status) { promise->setError(status); })
        .getAsync([](Status) {});

    return std::move(completionPf.future);
}

namespace {

// Health observer registration.
MONGO_INITIALIZER(ConfigServerHealthObserver)(InitializerContext*) {
    HealthObserverRegistration::registerObserverFactory([](ServiceContext* svcCtx) {
        return std::make_unique<ConfigServerHealthObserver>(svcCtx);
    });
}

}  // namespace

}  // namespace process_health
}  // namespace mongo
