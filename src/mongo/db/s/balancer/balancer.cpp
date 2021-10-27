
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/balancer.h"

#include <algorithm>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
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

const auto getBalancer = ServiceContext::declareDecoration<std::unique_ptr<Balancer>>();

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
        builder.append("errorOccured", _errMsg.is_initialized());

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
    auto&& vii = VersionInfoInterface::instance();

    bool isMultiVersion = false;
    for (const auto& stat : clusterStats) {
        if (!vii.isSameMajorVersion(stat.mongoVersion.c_str())) {
            isMultiVersion = true;
            break;
        }
    }

    // If we're all the same version, don't message
    if (!isMultiVersion)
        return;

    StringBuilder sb;
    sb << "Multi version cluster detected. Local version: " << vii.version()
       << ", shard versions: ";

    for (const auto& stat : clusterStats) {
        sb << stat.shardId << " is at " << stat.mongoVersion << "; ";
    }

    warning() << sb.str();
}

}  // namespace

Balancer::Balancer(ServiceContext* serviceContext)
    : _balancedLastTime(0),
      _random(std::random_device{}()),
      _clusterStats(stdx::make_unique<ClusterStatisticsImpl>(_random)),
      _chunkSelectionPolicy(
          stdx::make_unique<BalancerChunkSelectionPolicyImpl>(_clusterStats.get(), _random)),
      _migrationManager(serviceContext) {}

Balancer::~Balancer() {
    // The balancer thread must have been stopped
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    invariant(_state == kStopped);
}

void Balancer::create(ServiceContext* serviceContext) {
    invariant(!getBalancer(serviceContext));
    getBalancer(serviceContext) = stdx::make_unique<Balancer>(serviceContext);
}

Balancer* Balancer::get(ServiceContext* serviceContext) {
    return getBalancer(serviceContext).get();
}

Balancer* Balancer::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

void Balancer::initiateBalancer(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    invariant(_state == kStopped);
    _state = kRunning;

    _migrationManager.startRecoveryAndAcquireDistLocks(opCtx);

    invariant(!_thread.joinable());
    invariant(!_threadOperationContext);
    _thread = stdx::thread([this] { _mainThread(); });
}

void Balancer::interruptBalancer() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (_state != kRunning)
        return;

    _state = kStopping;

    // Interrupt the balancer thread if it has been started. We are guaranteed that the operation
    // context of that thread is still alive, because we hold the balancer mutex.
    if (_threadOperationContext) {
        stdx::lock_guard<Client> scopedClientLock(*_threadOperationContext->getClient());
        _threadOperationContext->markKilled(ErrorCodes::InterruptedDueToReplStateChange);
    }

    // Schedule a separate thread to shutdown the migration manager in order to avoid deadlock with
    // replication step down
    invariant(!_migrationManagerInterruptThread.joinable());
    _migrationManagerInterruptThread =
        stdx::thread([this] { _migrationManager.interruptAndDisableMigrations(); });

    _condVar.notify_all();
}

void Balancer::waitForBalancerToStop() {
    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        if (_state == kStopped)
            return;

        invariant(_state == kStopping);
        invariant(_thread.joinable());
    }

    _thread.join();

    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    _state = kStopped;
    _thread = {};

    LOG(1) << "Balancer thread terminated";
}

void Balancer::joinCurrentRound(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> scopedLock(_mutex);
    const auto numRoundsAtStart = _numBalancerRounds;
    opCtx->waitForConditionOrInterrupt(_condVar, scopedLock, [&] {
        return !_inBalancerRound || _numBalancerRounds != numRoundsAtStart;
    });
}

Status Balancer::rebalanceSingleChunk(OperationContext* opCtx, const ChunkType& chunk) {
    auto migrateStatus = _chunkSelectionPolicy->selectSpecificChunkToMove(opCtx, chunk);
    if (!migrateStatus.isOK()) {
        return migrateStatus.getStatus();
    }

    auto migrateInfo = std::move(migrateStatus.getValue());
    if (!migrateInfo) {
        LOG(1) << "Unable to find more appropriate location for chunk " << redact(chunk.toString());
        return Status::OK();
    }

    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    Status refreshStatus = balancerConfig->refreshAndCheck(opCtx);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    return _migrationManager.executeManualMigration(opCtx,
                                                    *migrateInfo,
                                                    balancerConfig->getMaxChunkSizeBytes(),
                                                    balancerConfig->getSecondaryThrottle(),
                                                    balancerConfig->waitForDelete());
}

Status Balancer::moveSingleChunk(OperationContext* opCtx,
                                 const ChunkType& chunk,
                                 const ShardId& newShardId,
                                 uint64_t maxChunkSizeBytes,
                                 const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                 bool waitForDelete) {
    auto moveAllowedStatus = _chunkSelectionPolicy->checkMoveAllowed(opCtx, chunk, newShardId);
    if (!moveAllowedStatus.isOK()) {
        return moveAllowedStatus;
    }

    return _migrationManager.executeManualMigration(
        opCtx, MigrateInfo(newShardId, chunk), maxChunkSizeBytes, secondaryThrottle, waitForDelete);
}

void Balancer::report(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    balancerConfig->refreshAndCheck(opCtx).transitional_ignore();

    const auto mode = balancerConfig->getBalancerMode();

    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    builder->append("mode", BalancerSettingsType::kBalancerModes[mode]);
    builder->append("inBalancerRound", _inBalancerRound);
    builder->append("numBalancerRounds", _numBalancerRounds);
}

void Balancer::_mainThread() {
    Client::initThread("Balancer");
    auto opCtx = cc().makeOperationContext();
    auto shardingContext = Grid::get(opCtx.get());

    log() << "CSRS balancer is starting";

    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        _threadOperationContext = opCtx.get();
    }

    const Seconds kInitBackoffInterval(10);

    auto balancerConfig = shardingContext->getBalancerConfiguration();
    while (!_stopRequested()) {
        Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
        if (!refreshStatus.isOK()) {
            warning() << "Balancer settings could not be loaded and will be retried in "
                      << durationCount<Seconds>(kInitBackoffInterval) << " seconds"
                      << causedBy(refreshStatus);

            _sleepFor(opCtx.get(), kInitBackoffInterval);
            continue;
        }

        break;
    }

    log() << "CSRS balancer thread is recovering";

    _migrationManager.finishRecovery(opCtx.get(),
                                     balancerConfig->getMaxChunkSizeBytes(),
                                     balancerConfig->getSecondaryThrottle());

    log() << "CSRS balancer thread is recovered";

    // Main balancer loop
    while (!_stopRequested()) {
        BalanceRoundDetails roundDetails;

        _beginRound(opCtx.get());

        try {
            shardingContext->shardRegistry()->reload(opCtx.get());

            uassert(13258, "oids broken after resetting!", _checkOIDs(opCtx.get()));

            Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
            if (!refreshStatus.isOK()) {
                warning() << "Skipping balancing round" << causedBy(refreshStatus);
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            if (!balancerConfig->shouldBalance()) {
                LOG(1) << "Skipping balancing round because balancing is disabled";
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            {
                LOG(1) << "*** start balancing round. "
                       << "waitForDelete: " << balancerConfig->waitForDelete()
                       << ", secondaryThrottle: "
                       << balancerConfig->getSecondaryThrottle().toBSON();

                OCCASIONALLY warnOnMultiVersion(
                    uassertStatusOK(_clusterStats->getStats(opCtx.get())));

                Status status = _splitChunksIfNeeded(opCtx.get());
                if (!status.isOK()) {
                    warning() << "Failed to split chunks" << causedBy(status);
                } else {
                    LOG(1) << "Done enforcing tag range boundaries.";
                }

                const auto candidateChunks = uassertStatusOK(
                    _chunkSelectionPolicy->selectChunksToMove(opCtx.get(), _balancedLastTime));

                if (candidateChunks.empty()) {
                    LOG(1) << "no need to move any chunk";
                    _balancedLastTime = false;
                } else {
                    _balancedLastTime = _moveChunks(opCtx.get(), candidateChunks);

                    roundDetails.setSucceeded(static_cast<int>(candidateChunks.size()),
                                              _balancedLastTime);

                    shardingContext->catalogClient()
                        ->logAction(opCtx.get(), "balancer.round", "", roundDetails.toBSON())
                        .ignore();
                }

                LOG(1) << "*** End of balancing round";
            }


            auto balancerInterval = [&]() -> Milliseconds {
                MONGO_FAIL_POINT_BLOCK(overrideBalanceRoundInterval, data) {
                    int interval = data.getData()["intervalMs"].numberInt();
                    log() << "overrideBalanceRoundInterval: using shorter balancing interval: "
                          << interval << "ms";

                    return Milliseconds(interval);
                }

                return _balancedLastTime ? kShortBalanceRoundInterval
                                         : kBalanceRoundDefaultInterval;
            }();

            _endRound(opCtx.get(), balancerInterval);
        } catch (const std::exception& e) {
            log() << "caught exception while doing balance: " << e.what();

            // Just to match the opening statement if in log level 1
            LOG(1) << "*** End of balancing round";

            // This round failed, tell the world!
            roundDetails.setFailed(e.what());

            shardingContext->catalogClient()
                ->logAction(opCtx.get(), "balancer.round", "", roundDetails.toBSON())
                .transitional_ignore();

            // Sleep a fair amount before retrying because of the error
            _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
        }
    }

    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        invariant(_state == kStopping);
        invariant(_migrationManagerInterruptThread.joinable());
    }

    _migrationManagerInterruptThread.join();
    _migrationManager.drainActiveMigrations();

    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        _migrationManagerInterruptThread = {};
        _threadOperationContext = nullptr;
    }

    log() << "CSRS balancer is now stopped";
}

bool Balancer::_stopRequested() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    return (_state != kRunning);
}

void Balancer::_beginRound(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _inBalancerRound = true;
    _condVar.notify_all();
}

void Balancer::_endRound(OperationContext* opCtx, Milliseconds waitTimeout) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _inBalancerRound = false;
        _numBalancerRounds++;
        _condVar.notify_all();
    }

    MONGO_IDLE_THREAD_BLOCK;
    _sleepFor(opCtx, waitTimeout);
}

void Balancer::_sleepFor(OperationContext* opCtx, Milliseconds waitTimeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condVar.wait_for(lock, waitTimeout.toSystemDuration(), [&] { return _state != kRunning; });
}

bool Balancer::_checkOIDs(OperationContext* opCtx) {
    auto shardingContext = Grid::get(opCtx);

    vector<ShardId> all;
    shardingContext->shardRegistry()->getAllShardIdsNoReload(&all);

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
                                                Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(result.commandStatus);
        BSONObj f = std::move(result.response);

        if (f["oidMachine"].isNumber()) {
            int x = f["oidMachine"].numberInt();
            if (oids.count(x) == 0) {
                oids[x] = shardId;
            } else {
                log() << "error: 2 machines have " << x << " as oid machine piece: " << shardId
                      << " and " << oids[x];

                result = uassertStatusOK(s->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    "admin",
                    BSON("features" << 1 << "oidReset" << 1),
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
                            Shard::RetryPolicy::kIdempotent));
                    uassertStatusOK(result.commandStatus);
                }

                return false;
            }
        } else {
            log() << "warning: oidMachine not set on: " << s->toString();
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

        auto cm = routingInfoStatus.getValue().cm();

        auto splitStatus =
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  splitInfo.shardId,
                                                  splitInfo.nss,
                                                  cm->getShardKeyPattern(),
                                                  splitInfo.collectionVersion,
                                                  ChunkRange(splitInfo.minKey, splitInfo.maxKey),
                                                  splitInfo.splitKeys);
        if (!splitStatus.isOK()) {
            warning() << "Failed to split chunk " << redact(splitInfo.toString())
                      << causedBy(redact(splitStatus.getStatus()));
        }
    }

    return Status::OK();
}

int Balancer::_moveChunks(OperationContext* opCtx,
                          const BalancerChunkSelectionPolicy::MigrateInfoVector& candidateChunks) {
    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

    // If the balancer was disabled since we started this round, don't start new chunk moves
    if (_stopRequested() || !balancerConfig->shouldBalance()) {
        LOG(1) << "Skipping balancing round because balancer was stopped";
        return 0;
    }

    auto migrationStatuses =
        _migrationManager.executeMigrationsForAutoBalance(opCtx,
                                                          candidateChunks,
                                                          balancerConfig->getMaxChunkSizeBytes(),
                                                          balancerConfig->getSecondaryThrottle(),
                                                          balancerConfig->waitForDelete());

    int numChunksProcessed = 0;

    for (const auto& migrationStatusEntry : migrationStatuses) {
        const Status& status = migrationStatusEntry.second;
        if (status.isOK()) {
            numChunksProcessed++;
            continue;
        }

        const MigrationIdentifier& migrationId = migrationStatusEntry.first;

        const auto requestIt = std::find_if(candidateChunks.begin(),
                                            candidateChunks.end(),
                                            [&migrationId](const MigrateInfo& migrateInfo) {
                                                return migrateInfo.getName() == migrationId;
                                            });
        invariant(requestIt != candidateChunks.end());

        if (status == ErrorCodes::ChunkTooBig) {
            numChunksProcessed++;

            log() << "Performing a split because migration " << redact(requestIt->toString())
                  << " failed for size reasons" << causedBy(redact(status));

            _splitOrMarkJumbo(opCtx, requestIt->nss, requestIt->minKey);
            continue;
        }

        log() << "Balancer move " << redact(requestIt->toString()) << " failed"
              << causedBy(redact(status));
    }

    return numChunksProcessed;
}

void Balancer::_splitOrMarkJumbo(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& minKey) {
    auto routingInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss));
    const auto cm = routingInfo.cm().get();

    auto chunk = cm->findIntersectingChunkWithSimpleCollation(minKey);

    try {
        const auto splitPoints = uassertStatusOK(shardutil::selectChunkSplitPoints(
            opCtx,
            chunk.getShardId(),
            nss,
            cm->getShardKeyPattern(),
            ChunkRange(chunk.getMin(), chunk.getMax()),
            Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes()));

        uassert(ErrorCodes::CannotSplit, "No split points found", !splitPoints.empty());

        uassertStatusOK(
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  chunk.getShardId(),
                                                  nss,
                                                  cm->getShardKeyPattern(),
                                                  cm->getVersion(),
                                                  ChunkRange(chunk.getMin(), chunk.getMax()),
                                                  splitPoints));
    } catch (const DBException&) {
        log() << "Marking chunk " << redact(chunk.toString()) << " as jumbo.";

        chunk.markAsJumbo();

        const std::string chunkName = ChunkType::genID(nss, chunk.getMin());

        auto status = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
            opCtx,
            ChunkType::ConfigNS,
            BSON(ChunkType::name(chunkName)),
            BSON("$set" << BSON(ChunkType::jumbo(true))),
            false,
            ShardingCatalogClient::kMajorityWriteConcern);
        if (!status.isOK()) {
            log() << "Couldn't set jumbo for chunk: " << redact(chunkName)
                  << causedBy(redact(status.getStatus()));
        }
    }
}

}  // namespace mongo
