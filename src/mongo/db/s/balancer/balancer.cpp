/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/balancer.h"

#include <algorithm>
#include <memory>
#include <pcrecpp.h>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/s/balancer/balancer_defragmentation_policy_impl.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_config_server_parameters_gen.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/balancer_collection_status_gen.h"
#include "mongo/s/request_types/configure_collection_balancing_gen.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

using std::map;
using std::string;
using std::vector;

namespace {

MONGO_FAIL_POINT_DEFINE(overrideBalanceRoundInterval);

const Seconds kBalanceRoundDefaultInterval(10);

// Sleep between balancer rounds in the case where the last round found some chunks which needed to
// be balanced. This value should be set sufficiently low so that imbalanced clusters will quickly
// reach balanced state, but setting it too low may cause CRUD operations to start failing due to
// not being able to establish a stable shard version.
const Seconds kShortBalanceRoundInterval(1);

/**
 * Balancer status response
 */
static constexpr StringData kBalancerPolicyStatusDraining = "draining"_sd;
static constexpr StringData kBalancerPolicyStatusZoneViolation = "zoneViolation"_sd;
static constexpr StringData kBalancerPolicyStatusChunksImbalance = "chunksImbalance"_sd;
static constexpr StringData kBalancerPolicyStatusDefragmentingChunks = "defragmentingChunks"_sd;

/**
 * Utility class to generate timing and statistics for a single balancer round.
 */
class BalanceRoundDetails {
public:
    BalanceRoundDetails() : _executionTimer() {}

    void setSucceeded(int candidateChunks, int chunksMoved) {
        invariant(!_errMsg);
        _candidateChunks = candidateChunks;
        _chunksMoved = chunksMoved;
    }

    void setFailed(const string& errMsg) {
        _errMsg = errMsg;
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("executionTimeMillis", _executionTimer.millis());
        builder.append("errorOccurred", _errMsg.is_initialized());

        if (_errMsg) {
            builder.append("errmsg", *_errMsg);
        } else {
            builder.append("candidateChunks", _candidateChunks);
            builder.append("chunksMoved", _chunksMoved);
        }
        return builder.obj();
    }

private:
    const Timer _executionTimer;

    // Set only on success
    int _candidateChunks{0};
    int _chunksMoved{0};

    // Set only on failure
    boost::optional<string> _errMsg;
};

/**
 * Occasionally prints a log message with shard versions if the versions are not the same
 * in the cluster.
 */
void warnOnMultiVersion(const vector<ClusterStatistics::ShardStatistics>& clusterStats) {
    static const auto& majorMinorRE = *new pcrecpp::RE(R"re(^(\d+)\.(\d+)\.)re");
    auto&& vii = VersionInfoInterface::instance();
    auto hasMyVersion = [&](auto&& stat) {
        int major;
        int minor;
        return majorMinorRE.PartialMatch(pcrecpp::StringPiece(stat.mongoVersion), &major, &minor) &&
            major == vii.majorVersion() && minor == vii.minorVersion();
    };

    // If we're all the same version, don't message
    if (std::all_of(clusterStats.begin(), clusterStats.end(), hasMyVersion))
        return;

    BSONObjBuilder shardVersions;
    for (const auto& stat : clusterStats) {
        shardVersions << stat.shardId << stat.mongoVersion;
    }

    LOGV2_WARNING(21875,
                  "Multiversion cluster detected",
                  "localVersion"_attr = vii.version(),
                  "shardVersions"_attr = shardVersions.done());
}

Status processManualMigrationOutcome(OperationContext* opCtx,
                                     const BSONObj& chunkMin,
                                     const NamespaceString& nss,
                                     const ShardId& destination,
                                     Status outcome) {
    // Since the commands scheduler uses a separate thread to remotely execute a
    // request, the resulting clusterTime needs to be explicitly retrieved and set on the
    // original context of the requestor to ensure it will be propagated back to the router.
    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
    replClient.setLastOpToSystemLastOpTime(opCtx);

    if (outcome.isOK()) {
        return outcome;
    }

    auto swCM =
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss);
    if (!swCM.isOK()) {
        return swCM.getStatus();
    }

    const auto currentChunkInfo =
        swCM.getValue().findIntersectingChunkWithSimpleCollation(chunkMin);
    if (currentChunkInfo.getShardId() == destination &&
        outcome != ErrorCodes::BalancerInterrupted) {
        // Migration calls can be interrupted after the metadata is committed but before the command
        // finishes the waitForDelete stage. Any failovers, therefore, must always cause the
        // moveChunk command to be retried so as to assure that the waitForDelete promise of a
        // successful command has been fulfilled.
        LOGV2(6036622,
              "Migration outcome is not OK, but the transaction was committed. Returning success");
        outcome = Status::OK();
    }
    return outcome;
}

const auto _balancerDecoration = ServiceContext::declareDecoration<Balancer>();

const ReplicaSetAwareServiceRegistry::Registerer<Balancer> _balancerRegisterer("Balancer");

}  // namespace

Balancer* Balancer::get(ServiceContext* serviceContext) {
    return &_balancerDecoration(serviceContext);
}

Balancer* Balancer::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

Balancer::Balancer()
    : _balancedLastTime(0),
      _random(std::random_device{}()),
      _clusterStats(std::make_unique<ClusterStatisticsImpl>(_random)),
      _chunkSelectionPolicy(
          std::make_unique<BalancerChunkSelectionPolicyImpl>(_clusterStats.get(), _random)),
      _commandScheduler(std::make_unique<BalancerCommandsSchedulerImpl>()),
      _defragmentationPolicy(
          std::make_unique<BalancerDefragmentationPolicyImpl>(_clusterStats.get(), _random)) {}

Balancer::~Balancer() {
    // Terminate the balancer thread so it doesn't leak memory.
    interruptBalancer();
    waitForBalancerToStop();
}

void Balancer::onStepUpBegin(OperationContext* opCtx, long long term) {
    // Before starting step-up, ensure the balancer is ready to start. Specifically, that the
    // balancer is actually stopped, because it may still be in the process of stopping if this
    // node was previously primary.
    waitForBalancerToStop();
}

void Balancer::onStepUpComplete(OperationContext* opCtx, long long term) {
    initiateBalancer(opCtx);
}

void Balancer::onStepDown() {
    interruptBalancer();
}

void Balancer::onBecomeArbiter() {
    // The Balancer is only active on config servers, and arbiters are not permitted in config
    // server replica sets.
    MONGO_UNREACHABLE;
}

void Balancer::initiateBalancer(OperationContext* opCtx) {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    invariant(_state == kStopped);
    _state = kRunning;

    invariant(!_thread.joinable());
    invariant(!_actionStreamConsumerThread.joinable());
    invariant(!_threadOperationContext);
    _thread = stdx::thread([this] { _mainThread(); });
}

void Balancer::interruptBalancer() {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    if (_state != kRunning)
        return;

    _state = kStopping;
    _thread.detach();

    // Interrupt the balancer thread if it has been started. We are guaranteed that the operation
    // context of that thread is still alive, because we hold the balancer mutex.
    if (_threadOperationContext) {
        stdx::lock_guard<Client> scopedClientLock(*_threadOperationContext->getClient());
        _threadOperationContext->markKilled(ErrorCodes::InterruptedDueToReplStateChange);
    }

    _condVar.notify_all();
}

void Balancer::waitForBalancerToStop() {
    stdx::unique_lock<Latch> scopedLock(_mutex);

    _joinCond.wait(scopedLock, [this] { return _state == kStopped; });
}

void Balancer::joinCurrentRound(OperationContext* opCtx) {
    stdx::unique_lock<Latch> scopedLock(_mutex);
    const auto numRoundsAtStart = _numBalancerRounds;
    opCtx->waitForConditionOrInterrupt(_condVar, scopedLock, [&] {
        return !_inBalancerRound || _numBalancerRounds != numRoundsAtStart;
    });
}

Balancer::ScopedPauseBalancerRequest Balancer::requestPause() {
    return ScopedPauseBalancerRequest(this);
}

Status Balancer::rebalanceSingleChunk(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const ChunkType& chunk) {
    auto migrateStatus = _chunkSelectionPolicy->selectSpecificChunkToMove(opCtx, nss, chunk);
    if (!migrateStatus.isOK()) {
        return migrateStatus.getStatus();
    }

    auto migrateInfo = std::move(migrateStatus.getValue());
    if (!migrateInfo) {
        LOGV2_DEBUG(21854,
                    1,
                    "Unable to find more appropriate location for chunk {chunk}",
                    "Unable to find more appropriate location for chunk",
                    "chunk"_attr = redact(chunk.toString()));
        return Status::OK();
    }

    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    Status refreshStatus = balancerConfig->refreshAndCheck(opCtx);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    auto coll = Grid::get(opCtx)->catalogClient()->getCollection(
        opCtx, nss, repl::ReadConcernLevel::kMajorityReadConcern);
    auto maxChunkSize =
        coll.getMaxChunkSizeBytes().value_or(balancerConfig->getMaxChunkSizeBytes());

    MoveChunkSettings settings(
        maxChunkSize, balancerConfig->getSecondaryThrottle(), balancerConfig->waitForDelete());
    auto response =
        _commandScheduler
            ->requestMoveChunk(opCtx, *migrateInfo, settings, true /* issuedByRemoteUser */)
            .getNoThrow();
    return processManualMigrationOutcome(opCtx, chunk.getMin(), nss, migrateInfo->to, response);
}

Status Balancer::moveSingleChunk(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const ChunkType& chunk,
                                 const ShardId& newShardId,
                                 const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                 bool waitForDelete,
                                 bool forceJumbo) {
    auto moveAllowedStatus = _chunkSelectionPolicy->checkMoveAllowed(opCtx, chunk, newShardId);
    if (!moveAllowedStatus.isOK()) {
        return moveAllowedStatus;
    }

    auto coll = Grid::get(opCtx)->catalogClient()->getCollection(
        opCtx, nss, repl::ReadConcernLevel::kMajorityReadConcern);
    auto maxChunkSize = coll.getMaxChunkSizeBytes().value_or(-1);
    if (maxChunkSize <= 0) {
        auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
        Status refreshStatus = balancerConfig->refreshAndCheck(opCtx);
        if (!refreshStatus.isOK()) {
            return refreshStatus;
        }

        maxChunkSize = balancerConfig->getMaxChunkSizeBytes();
    }

    MoveChunkSettings settings(maxChunkSize, secondaryThrottle, waitForDelete);
    MigrateInfo migrateInfo(newShardId,
                            nss,
                            chunk,
                            forceJumbo ? MoveChunkRequest::ForceJumbo::kForceManual
                                       : MoveChunkRequest::ForceJumbo::kDoNotForce,
                            MigrateInfo::chunksImbalance);
    auto response =
        _commandScheduler
            ->requestMoveChunk(opCtx, migrateInfo, settings, true /* issuedByRemoteUser */)
            .getNoThrow();
    return processManualMigrationOutcome(opCtx, chunk.getMin(), nss, newShardId, response);
}

void Balancer::report(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    balancerConfig->refreshAndCheck(opCtx).ignore();

    const auto mode = balancerConfig->getBalancerMode();

    stdx::lock_guard<Latch> scopedLock(_mutex);
    builder->append("mode", BalancerSettingsType::kBalancerModes[mode]);
    builder->append("inBalancerRound", _inBalancerRound);
    builder->append("numBalancerRounds", _numBalancerRounds);
}

void Balancer::_consumeActionStreamLoop() {
    Client::initThread("BalancerSecondary");
    auto applyThrottling = [lastActionTime = Date_t::fromMillisSinceEpoch(0)]() mutable {
        const Milliseconds throttle{chunkDefragmentationThrottlingMS.load()};
        auto timeSinceLastAction = Date_t::now() - lastActionTime;
        if (throttle > timeSinceLastAction) {
            sleepFor(throttle - timeSinceLastAction);
        }
        lastActionTime = Date_t::now();
    };
    auto opCtx = cc().makeOperationContext();
    executor::ScopedTaskExecutor executor(
        Grid::get(opCtx.get())->getExecutorPool()->getFixedExecutor());
    while (!_stopRequested()) {
        // Blocking call
        DefragmentationAction action =
            _defragmentationPolicy->getNextStreamingAction(opCtx.get()).get();
        // Non-blocking call, assumes the requests are returning a SemiFuture<>
        stdx::visit(
            visit_helper::Overloaded{
                [&](MergeInfo&& mergeAction) {
                    applyThrottling();
                    auto result =
                        _commandScheduler
                            ->requestMergeChunks(opCtx.get(),
                                                 mergeAction.nss,
                                                 mergeAction.shardId,
                                                 mergeAction.chunkRange,
                                                 mergeAction.collectionVersion)
                            .thenRunOn(*executor)
                            .onCompletion(
                                [this, command = std::move(mergeAction)](const Status& status) {
                                    ThreadClient tc(
                                        "BalancerDefragmentationPolicy::acknowledgeMergeResult",
                                        getGlobalServiceContext());
                                    auto opCtx = tc->makeOperationContext();
                                    _defragmentationPolicy->acknowledgeMergeResult(
                                        opCtx.get(), command, status);
                                });
                },
                [&](DataSizeInfo&& dataSizeAction) {
                    auto result =
                        _commandScheduler
                            ->requestDataSize(opCtx.get(),
                                              dataSizeAction.nss,
                                              dataSizeAction.shardId,
                                              dataSizeAction.chunkRange,
                                              dataSizeAction.version,
                                              dataSizeAction.keyPattern,
                                              dataSizeAction.estimatedValue)
                            .thenRunOn(*executor)
                            .onCompletion([this, command = std::move(dataSizeAction)](
                                              const StatusWith<DataSizeResponse>& swDataSize) {
                                ThreadClient tc(
                                    "BalancerDefragmentationPolicy::acknowledgeDataSizeResult",
                                    getGlobalServiceContext());
                                auto opCtx = tc->makeOperationContext();
                                _defragmentationPolicy->acknowledgeDataSizeResult(
                                    opCtx.get(), command, swDataSize);
                            });
                },
                [&](AutoSplitVectorInfo&& splitVectorAction) {
                    auto result =
                        _commandScheduler
                            ->requestAutoSplitVector(opCtx.get(),
                                                     splitVectorAction.nss,
                                                     splitVectorAction.shardId,
                                                     splitVectorAction.keyPattern,
                                                     splitVectorAction.minKey,
                                                     splitVectorAction.maxKey,
                                                     splitVectorAction.maxChunkSizeBytes)
                            .thenRunOn(*executor)
                            .onCompletion([this, command = std::move(splitVectorAction)](
                                              const StatusWith<std::vector<BSONObj>>&
                                                  swSplitPoints) {
                                ThreadClient tc(
                                    "BalancerDefragmentationPolicy::acknowledgeSplitVectorResult",
                                    getGlobalServiceContext());
                                auto opCtx = tc->makeOperationContext();
                                _defragmentationPolicy->acknowledgeAutoSplitVectorResult(
                                    opCtx.get(), command, swSplitPoints);
                            });
                },
                [&](SplitInfoWithKeyPattern&& splitAction) {
                    applyThrottling();
                    auto result =
                        _commandScheduler
                            ->requestSplitChunk(opCtx.get(),
                                                splitAction.info.nss,
                                                splitAction.info.shardId,
                                                splitAction.info.collectionVersion,
                                                splitAction.keyPattern,
                                                splitAction.info.minKey,
                                                splitAction.info.maxKey,
                                                splitAction.info.splitKeys)
                            .thenRunOn(*executor)
                            .onCompletion(
                                [this, command = std::move(splitAction)](const Status& status) {
                                    ThreadClient tc(
                                        "BalancerDefragmentationPolicy::acknowledgeSplitResult",
                                        getGlobalServiceContext());
                                    auto opCtx = tc->makeOperationContext();
                                    _defragmentationPolicy->acknowledgeSplitResult(
                                        opCtx.get(), command, status);
                                });
                },
                [](MigrateInfo&& _) {
                    uasserted(ErrorCodes::BadValue,
                              "Migrations cannot be processed as Streaming Actions");
                },
                [](EndOfActionStream eoa) {}},
            std::move(action));
    }
}

void Balancer::_mainThread() {
    ON_BLOCK_EXIT([this] {
        stdx::lock_guard<Latch> scopedLock(_mutex);

        _state = kStopped;
        _joinCond.notify_all();

        LOGV2_DEBUG(21855, 1, "Balancer thread terminated");
    });

    Client::initThread("Balancer");
    auto opCtx = cc().makeOperationContext();
    auto shardingContext = Grid::get(opCtx.get());

    LOGV2(21856, "CSRS balancer is starting");

    {
        stdx::lock_guard<Latch> scopedLock(_mutex);
        _threadOperationContext = opCtx.get();
    }

    const Seconds kInitBackoffInterval(10);

    auto balancerConfig = shardingContext->getBalancerConfiguration();
    while (!_stopRequested()) {
        Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
        if (!refreshStatus.isOK()) {
            LOGV2_WARNING(
                21876,
                "Balancer settings could not be loaded because of {error} and will be retried in "
                "{backoffInterval}",
                "Got error while refreshing balancer settings, will retry with a backoff",
                "backoffInterval"_attr = Milliseconds(kInitBackoffInterval),
                "error"_attr = refreshStatus);

            _sleepFor(opCtx.get(), kInitBackoffInterval);
            continue;
        }

        break;
    }

    LOGV2(6036605, "Starting command scheduler");

    _commandScheduler->start(
        opCtx.get(),
        MigrationsRecoveryDefaultValues(balancerConfig->getMaxChunkSizeBytes(),
                                        balancerConfig->getSecondaryThrottle()));

    _actionStreamConsumerThread = stdx::thread([&] { _consumeActionStreamLoop(); });

    LOGV2(6036606, "Balancer worker thread initialised. Entering main loop.");

    // Main balancer loop
    while (!_stopRequested()) {
        BalanceRoundDetails roundDetails;

        _beginRound(opCtx.get());

        try {
            shardingContext->shardRegistry()->reload(opCtx.get());

            uassert(13258, "oids broken after resetting!", _checkOIDs(opCtx.get()));

            Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
            if (!refreshStatus.isOK()) {
                LOGV2_WARNING(21877,
                              "Skipping balancing round due to {error}",
                              "Skipping balancing round",
                              "error"_attr = refreshStatus);
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }
            if (!balancerConfig->shouldBalance() || _stopOrPauseRequested()) {
                LOGV2_DEBUG(21859, 1, "Skipping balancing round because balancing is disabled");
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            {
                LOGV2_DEBUG(21860,
                            1,
                            "Start balancing round. waitForDelete: {waitForDelete}, "
                            "secondaryThrottle: {secondaryThrottle}",
                            "Start balancing round",
                            "waitForDelete"_attr = balancerConfig->waitForDelete(),
                            "secondaryThrottle"_attr =
                                balancerConfig->getSecondaryThrottle().toBSON());

                static Occasionally sampler;
                if (sampler.tick()) {
                    warnOnMultiVersion(uassertStatusOK(_clusterStats->getStats(opCtx.get())));
                }

                // Collect and apply up-to-date configuration values on the cluster collections.
                {
                    OperationContext* ctx = opCtx.get();
                    auto allCollections = Grid::get(ctx)->catalogClient()->getCollections(ctx, {});
                    for (const auto& coll : allCollections) {
                        _defragmentationPolicy->refreshCollectionDefragmentationStatus(ctx, coll);
                    }
                }

                Status status = _splitChunksIfNeeded(opCtx.get());
                if (!status.isOK()) {
                    LOGV2_WARNING(21878,
                                  "Failed to split chunks due to {error}",
                                  "Failed to split chunks",
                                  "error"_attr = status);
                } else {
                    LOGV2_DEBUG(21861, 1, "Done enforcing tag range boundaries.");
                }

                stdx::unordered_set<ShardId> usedShards;

                const auto chunksToDefragment =
                    _defragmentationPolicy->selectChunksToMove(opCtx.get(), &usedShards);

                const auto chunksToRebalance = uassertStatusOK(
                    _chunkSelectionPolicy->selectChunksToMove(opCtx.get(), &usedShards));

                if (chunksToRebalance.empty() && chunksToDefragment.empty()) {
                    LOGV2_DEBUG(21862, 1, "No need to move any chunk");
                    _balancedLastTime = 0;
                } else {
                    _balancedLastTime =
                        _moveChunks(opCtx.get(), chunksToRebalance, chunksToDefragment);

                    roundDetails.setSucceeded(
                        static_cast<int>(chunksToRebalance.size() + chunksToDefragment.size()),
                        _balancedLastTime);

                    ShardingLogging::get(opCtx.get())
                        ->logAction(opCtx.get(), "balancer.round", "", roundDetails.toBSON())
                        .ignore();
                }

                LOGV2_DEBUG(21863, 1, "End balancing round");
            }

            Milliseconds balancerInterval =
                _balancedLastTime ? kShortBalanceRoundInterval : kBalanceRoundDefaultInterval;

            overrideBalanceRoundInterval.execute([&](const BSONObj& data) {
                balancerInterval = Milliseconds(data["intervalMs"].numberInt());
                LOGV2(21864,
                      "overrideBalanceRoundInterval: using shorter balancing interval: "
                      "{balancerInterval}",
                      "overrideBalanceRoundInterval: using shorter balancing interval",
                      "balancerInterval"_attr = balancerInterval);
            });

            _endRound(opCtx.get(), balancerInterval);
        } catch (const DBException& e) {
            LOGV2(21865,
                  "caught exception while doing balance: {error}",
                  "Error while doing balance",
                  "error"_attr = e);

            // Just to match the opening statement if in log level 1
            LOGV2_DEBUG(21866, 1, "End balancing round");

            // This round failed, tell the world!
            roundDetails.setFailed(e.what());

            ShardingLogging::get(opCtx.get())
                ->logAction(opCtx.get(), "balancer.round", "", roundDetails.toBSON())
                .ignore();

            // Sleep a fair amount before retrying because of the error
            _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
        }
    }

    {
        stdx::lock_guard<Latch> scopedLock(_mutex);
        invariant(_state == kStopping);
    }

    _defragmentationPolicy->closeActionStream();

    _commandScheduler->stop();

    _actionStreamConsumerThread.join();


    {
        stdx::lock_guard<Latch> scopedLock(_mutex);
        _threadOperationContext = nullptr;
    }

    LOGV2(21867, "CSRS balancer is now stopped");
}

void Balancer::_addPauseRequest() {
    stdx::unique_lock<Latch> scopedLock(_mutex);
    ++_numPauseRequests;
}

void Balancer::_removePauseRequest() {
    stdx::unique_lock<Latch> scopedLock(_mutex);
    invariant(_numPauseRequests > 0);
    --_numPauseRequests;
}

bool Balancer::_stopRequested() {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    return (_state != kRunning);
}

bool Balancer::_stopOrPauseRequested() {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    return (_state != kRunning || _numPauseRequests > 0);
}

void Balancer::_beginRound(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lock(_mutex);
    _inBalancerRound = true;
    _condVar.notify_all();
}

void Balancer::_endRound(OperationContext* opCtx, Milliseconds waitTimeout) {
    {
        stdx::lock_guard<Latch> lock(_mutex);
        _inBalancerRound = false;
        _numBalancerRounds++;
        _condVar.notify_all();
    }

    MONGO_IDLE_THREAD_BLOCK;
    _sleepFor(opCtx, waitTimeout);
}

void Balancer::_sleepFor(OperationContext* opCtx, Milliseconds waitTimeout) {
    stdx::unique_lock<Latch> lock(_mutex);
    _condVar.wait_for(lock, waitTimeout.toSystemDuration(), [&] { return _state != kRunning; });
}

bool Balancer::_checkOIDs(OperationContext* opCtx) {
    auto shardingContext = Grid::get(opCtx);

    const auto all = shardingContext->shardRegistry()->getAllShardIdsNoReload();

    // map of OID machine ID => shardId
    map<int, ShardId> oids;

    for (const ShardId& shardId : all) {
        if (_stopRequested()) {
            return false;
        }

        auto shardStatus = shardingContext->shardRegistry()->getShard(opCtx, shardId);
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto s = shardStatus.getValue();

        auto result = uassertStatusOK(
            s->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                "admin",
                                                BSON("features" << 1),
                                                Seconds(30),
                                                Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(result.commandStatus);
        BSONObj f = std::move(result.response);

        if (f["oidMachine"].isNumber()) {
            int x = f["oidMachine"].numberInt();
            if (oids.count(x) == 0) {
                oids[x] = shardId;
            } else {
                LOGV2(21868,
                      "error: 2 machines have {oidMachine} as oid machine piece: {firstShardId} "
                      "and {secondShardId}",
                      "Two machines have the same oidMachine value",
                      "oidMachine"_attr = x,
                      "firstShardId"_attr = shardId,
                      "secondShardId"_attr = oids[x]);

                result = uassertStatusOK(s->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    "admin",
                    BSON("features" << 1 << "oidReset" << 1),
                    Seconds(30),
                    Shard::RetryPolicy::kIdempotent));
                uassertStatusOK(result.commandStatus);

                auto otherShardStatus = shardingContext->shardRegistry()->getShard(opCtx, oids[x]);
                if (otherShardStatus.isOK()) {
                    result = uassertStatusOK(
                        otherShardStatus.getValue()->runCommandWithFixedRetryAttempts(
                            opCtx,
                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                            "admin",
                            BSON("features" << 1 << "oidReset" << 1),
                            Seconds(30),
                            Shard::RetryPolicy::kIdempotent));
                    uassertStatusOK(result.commandStatus);
                }

                return false;
            }
        } else {
            LOGV2(21869,
                  "warning: oidMachine not set on: {shard}",
                  "warning: oidMachine not set on shard",
                  "shard"_attr = s->toString());
        }
    }

    return true;
}

Status Balancer::_splitChunksIfNeeded(OperationContext* opCtx) {
    auto chunksToSplitStatus = _chunkSelectionPolicy->selectChunksToSplit(opCtx);
    if (!chunksToSplitStatus.isOK()) {
        return chunksToSplitStatus.getStatus();
    }

    for (const auto& splitInfo : chunksToSplitStatus.getValue()) {
        auto routingInfoStatus =
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
                opCtx, splitInfo.nss);
        if (!routingInfoStatus.isOK()) {
            return routingInfoStatus.getStatus();
        }

        const auto& cm = routingInfoStatus.getValue();

        auto splitStatus =
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  splitInfo.shardId,
                                                  splitInfo.nss,
                                                  cm.getShardKeyPattern(),
                                                  splitInfo.collectionVersion.epoch(),
                                                  ChunkVersion::IGNORED() /*shardVersion*/,
                                                  ChunkRange(splitInfo.minKey, splitInfo.maxKey),
                                                  splitInfo.splitKeys);
        if (!splitStatus.isOK()) {
            LOGV2_WARNING(21879,
                          "Failed to split chunk {splitInfo} {error}",
                          "Failed to split chunk",
                          "splitInfo"_attr = redact(splitInfo.toString()),
                          "error"_attr = redact(splitStatus.getStatus()));
        }
    }

    return Status::OK();
}

int Balancer::_moveChunks(OperationContext* opCtx,
                          const MigrateInfoVector& chunksToRebalance,
                          const MigrateInfoVector& chunksToDefragment) {
    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    auto catalogClient = Grid::get(opCtx)->catalogClient();

    // If the balancer was disabled since we started this round, don't start new chunk moves
    if (_stopOrPauseRequested() || !balancerConfig->shouldBalance()) {
        LOGV2_DEBUG(21870, 1, "Skipping balancing round because balancer was stopped");
        return 0;
    }

    std::vector<std::pair<const MigrateInfo&, SemiFuture<void>>> rebalanceMigrationsAndResponses,
        defragmentationMigrationsAndResponses;
    auto requestMigration = [&](const MigrateInfo& migrateInfo) -> SemiFuture<void> {
        auto coll = Grid::get(opCtx)->catalogClient()->getCollection(
            opCtx, migrateInfo.nss, repl::ReadConcernLevel::kMajorityReadConcern);
        auto maxChunkSizeBytes =
            coll.getMaxChunkSizeBytes().value_or(balancerConfig->getMaxChunkSizeBytes());

        MoveChunkSettings settings(maxChunkSizeBytes,
                                   balancerConfig->getSecondaryThrottle(),
                                   balancerConfig->waitForDelete());
        return _commandScheduler->requestMoveChunk(opCtx, migrateInfo, settings);
    };

    for (const auto& rebalanceOp : chunksToRebalance) {
        rebalanceMigrationsAndResponses.emplace_back(rebalanceOp, requestMigration(rebalanceOp));
    }
    for (const auto& defragmentationOp : chunksToDefragment) {
        defragmentationMigrationsAndResponses.emplace_back(defragmentationOp,
                                                           requestMigration(defragmentationOp));
    }

    int numChunksProcessed = 0;
    for (const auto& [migrateInfo, futureStatus] : rebalanceMigrationsAndResponses) {
        auto status = futureStatus.getNoThrow();
        if (status.isOK()) {
            ++numChunksProcessed;
            continue;
        }

        // ChunkTooBig is returned by the source shard during the cloning phase if the migration
        // manager finds that the chunk is larger than some calculated size, the source shard is
        // *not* in draining mode, and the 'forceJumbo' balancer setting is 'kDoNotForce'.
        // ExceededMemoryLimit is returned when the transfer mods queue surpasses 500MB regardless
        // of whether the source shard is in draining mode or the value if the 'froceJumbo' balancer
        // setting.
        if (status == ErrorCodes::ChunkTooBig || status == ErrorCodes::ExceededMemoryLimit) {
            ++numChunksProcessed;

            LOGV2(21871,
                  "Migration {migrateInfo} failed with {error}, going to try splitting the chunk",
                  "Migration failed, going to try splitting the chunk",
                  "migrateInfo"_attr = redact(migrateInfo.toString()),
                  "error"_attr = redact(status));

            const CollectionType collection = catalogClient->getCollection(
                opCtx, migrateInfo.uuid, repl::ReadConcernLevel::kMajorityReadConcern);

            ShardingCatalogManager::get(opCtx)->splitOrMarkJumbo(
                opCtx, collection.getNss(), migrateInfo.minKey);
            continue;
        }

        LOGV2(21872,
              "Migration {migrateInfo} failed with {error}",
              "Migration failed",
              "migrateInfo"_attr = redact(migrateInfo.toString()),
              "error"_attr = redact(status));
    }

    for (const auto& [migrateInfo, futureStatus] : defragmentationMigrationsAndResponses) {
        auto status = futureStatus.getNoThrow();
        if (status.isOK()) {
            ++numChunksProcessed;
        }
        _defragmentationPolicy->acknowledgeMoveResult(opCtx, migrateInfo, status);
    }


    return numChunksProcessed;
}

void Balancer::notifyPersistedBalancerSettingsChanged() {
    stdx::unique_lock<Latch> lock(_mutex);
    _condVar.notify_all();
}

void Balancer::abortCollectionDefragmentation(OperationContext* opCtx, const NamespaceString& nss) {
    _defragmentationPolicy->abortCollectionDefragmentation(opCtx, nss);
}

BalancerCollectionStatusResponse Balancer::getBalancerStatusForNs(OperationContext* opCtx,
                                                                  const NamespaceString& ns) {
    CollectionType coll;
    try {
        coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, ns, {});
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        uasserted(ErrorCodes::NamespaceNotSharded, "Collection unsharded or undefined");
    }

    const auto maxChunkSizeMB = [&]() -> int64_t {
        int64_t value = 0;
        if (const auto& collOverride = coll.getMaxChunkSizeBytes(); collOverride.is_initialized()) {
            value = *collOverride;
        } else {
            auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
            uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));
            value = balancerConfig->getMaxChunkSizeBytes();
        }
        return value / (1024 * 1024);
    }();
    BalancerCollectionStatusResponse response(maxChunkSizeMB, true /*balancerCompliant*/);
    auto setViolationOnResponse = [&response](const StringData& reason,
                                              const boost::optional<BSONObj>& details =
                                                  boost::none) {
        response.setBalancerCompliant(false);
        response.setFirstComplianceViolation(reason);
        response.setDetails(details);
    };

    bool isDefragmenting = coll.getDefragmentCollection();
    if (isDefragmenting) {
        setViolationOnResponse(kBalancerPolicyStatusDefragmentingChunks,
                               _defragmentationPolicy->reportProgressOn(coll.getUuid()));
        return response;
    }

    auto splitChunks = uassertStatusOK(_chunkSelectionPolicy->selectChunksToSplit(opCtx, ns));
    if (!splitChunks.empty()) {
        setViolationOnResponse(kBalancerPolicyStatusZoneViolation);
        return response;
    }

    auto chunksToMove = uassertStatusOK(_chunkSelectionPolicy->selectChunksToMove(opCtx, ns));
    if (chunksToMove.empty()) {
        return response;
    }

    const auto& migrationInfo = chunksToMove.front();
    switch (migrationInfo.reason) {
        case MigrateInfo::drain:
            setViolationOnResponse(kBalancerPolicyStatusDraining);
            break;
        case MigrateInfo::zoneViolation:
            setViolationOnResponse(kBalancerPolicyStatusZoneViolation);
            break;
        case MigrateInfo::chunksImbalance:
            setViolationOnResponse(kBalancerPolicyStatusChunksImbalance);
            break;
    }

    return response;
}

}  // namespace mongo
