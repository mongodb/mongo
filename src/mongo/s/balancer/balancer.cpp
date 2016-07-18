/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/balancer/balancer.h"

#include <algorithm>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/balancer/cluster_statistics_impl.h"
#include "mongo/s/balancer/migration_manager.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

using std::map;
using std::string;
using std::vector;

namespace {

const Seconds kBalanceRoundDefaultInterval(10);
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
    bool isMultiVersion = false;
    for (const auto& stat : clusterStats) {
        if (!isSameMajorVersion(stat.mongoVersion.c_str())) {
            isMultiVersion = true;
            break;
        }
    }

    // If we're all the same version, don't message
    if (!isMultiVersion)
        return;

    StringBuilder sb;
    sb << "Multi version cluster detected. Local version: " << versionString
       << ", shard versions: ";

    for (const auto& stat : clusterStats) {
        sb << stat.shardId << " is at " << stat.mongoVersion << "; ";
    }

    warning() << sb.str();
}

}  // namespace

Balancer::Balancer()
    : _balancedLastTime(0),
      _chunkSelectionPolicy(stdx::make_unique<BalancerChunkSelectionPolicyImpl>(
          stdx::make_unique<ClusterStatisticsImpl>())),
      _clusterStats(stdx::make_unique<ClusterStatisticsImpl>()) {}

Balancer::~Balancer() {
    // The balancer thread must have been stopped
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    invariant(_state == kStopped);
}

void Balancer::create(ServiceContext* serviceContext) {
    invariant(!getBalancer(serviceContext));
    getBalancer(serviceContext) = stdx::make_unique<Balancer>();
}

Balancer* Balancer::get(ServiceContext* serviceContext) {
    return getBalancer(serviceContext).get();
}

Balancer* Balancer::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

Status Balancer::startThread(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    switch (_state) {
        case kStopping:
            return {ErrorCodes::ConflictingOperationInProgress,
                    "Sharding balancer is in currently being shut down"};
        case kStopped:
            invariant(!_thread.joinable());
            _state = kRunning;
            _thread = stdx::thread([this] { _mainThread(); });
        // Intentional fall through
        case kRunning:
            return Status::OK();
        default:
            MONGO_UNREACHABLE;
    }
}

void Balancer::stopThread() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (_state == kRunning) {
        _state = kStopping;
        _condVar.notify_all();
    }
}

void Balancer::joinThread() {
    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        if (_state == kStopped) {
            return;
        }

        invariant(_state == kStopping);
    }

    if (_thread.joinable()) {
        _thread.join();

        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        _state = kStopped;
        _thread = {};
    }
}

void Balancer::joinCurrentRound(OperationContext* txn) {
    stdx::unique_lock<stdx::mutex> scopedLock(_mutex);
    const auto numRoundsAtStart = _numBalancerRounds;
    _condVar.wait(scopedLock,
                  [&] { return !_inBalancerRound || _numBalancerRounds != numRoundsAtStart; });
}

Status Balancer::rebalanceSingleChunk(OperationContext* txn, const ChunkType& chunk) {
    auto migrateStatus = _chunkSelectionPolicy->selectSpecificChunkToMove(txn, chunk);
    if (!migrateStatus.isOK()) {
        return migrateStatus.getStatus();
    }

    auto migrateInfo = std::move(migrateStatus.getValue());
    if (!migrateInfo) {
        LOG(1) << "Unable to find more appropriate location for chunk " << redact(chunk.toString());
        return Status::OK();
    }

    auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();
    Status refreshStatus = balancerConfig->refreshAndCheck(txn);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    MigrationManager migrationManager;
    auto migrationStatuses = migrationManager.scheduleMigrations(
        txn,
        {MigrationManager::MigrationRequest(std::move(*migrateInfo),
                                            balancerConfig->getMaxChunkSizeBytes(),
                                            balancerConfig->getSecondaryThrottle(),
                                            balancerConfig->waitForDelete())});

    invariant(migrationStatuses.size() == 1);

    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, NamespaceString(chunk.getNS()));
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();
    cm->reload(txn);

    return migrationStatuses.begin()->second;
}

Status Balancer::moveSingleChunk(OperationContext* txn,
                                 const ChunkType& chunk,
                                 const ShardId& newShardId,
                                 uint64_t maxChunkSizeBytes,
                                 const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                 bool waitForDelete) {
    auto moveAllowedStatus = _chunkSelectionPolicy->checkMoveAllowed(txn, chunk, newShardId);
    if (!moveAllowedStatus.isOK()) {
        return moveAllowedStatus;
    }

    MigrationManager migrationManager;
    auto migrationStatuses = migrationManager.scheduleMigrations(
        txn,
        {MigrationManager::MigrationRequest(MigrateInfo(chunk.getNS(), newShardId, chunk),
                                            maxChunkSizeBytes,
                                            secondaryThrottle,
                                            waitForDelete)});

    invariant(migrationStatuses.size() == 1);

    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, NamespaceString(chunk.getNS()));
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();
    cm->reload(txn);

    return migrationStatuses.begin()->second;
}

void Balancer::report(OperationContext* txn, BSONObjBuilder* builder) {
    auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();
    balancerConfig->refreshAndCheck(txn);

    const auto mode = balancerConfig->getBalancerMode();

    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    builder->append("mode", BalancerSettingsType::kBalancerModes[mode]);
    builder->append("inBalancerRound", _inBalancerRound);
    builder->append("numBalancerRounds", _numBalancerRounds);
}

void Balancer::_mainThread() {
    Client::initThread("Balancer");

    // TODO (SERVER-24754): Balancer thread should only keep the operation context alive while it is
    // doing balancing
    const auto txn = cc().makeOperationContext();

    log() << "CSRS balancer is starting";

    const Seconds kInitBackoffInterval(60);

    // The balancer thread is holding the balancer during its entire lifetime
    boost::optional<DistLockManager::ScopedDistLock> scopedBalancerLock;

    // Take the balancer distributed lock
    while (!_stopRequested() && !scopedBalancerLock) {
        auto shardingContext = Grid::get(txn.get());
        auto scopedDistLock = shardingContext->catalogClient(txn.get())->distLock(
            txn.get(), "balancer", "CSRS Balancer");
        if (!scopedDistLock.isOK()) {
            warning() << "Balancer distributed lock could not be acquired and will be retried in "
                         "one minute"
                      << causedBy(scopedDistLock.getStatus());
            _sleepFor(txn.get(), kInitBackoffInterval);
            continue;
        }

        // Initialization and distributed lock acquisition succeeded
        scopedBalancerLock = std::move(scopedDistLock.getValue());
    }

    log() << "CSRS balancer thread is now running";

    // Main balancer loop
    while (!_stopRequested()) {
        auto shardingContext = Grid::get(txn.get());
        auto balancerConfig = shardingContext->getBalancerConfiguration();

        BalanceRoundDetails roundDetails;

        try {
            _beginRound(txn.get());

            shardingContext->shardRegistry()->reload(txn.get());

            uassert(13258, "oids broken after resetting!", _checkOIDs(txn.get()));

            Status refreshStatus = balancerConfig->refreshAndCheck(txn.get());
            if (!refreshStatus.isOK()) {
                warning() << "Skipping balancing round" << causedBy(refreshStatus);
                _endRound(txn.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            if (!balancerConfig->shouldBalance()) {
                LOG(1) << "Skipping balancing round because balancing is disabled";
                _endRound(txn.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            {
                LOG(1) << "*** start balancing round. "
                       << "waitForDelete: " << balancerConfig->waitForDelete()
                       << ", secondaryThrottle: "
                       << balancerConfig->getSecondaryThrottle().toBSON();

                OCCASIONALLY warnOnMultiVersion(
                    uassertStatusOK(_clusterStats->getStats(txn.get())));

                Status status = _enforceTagRanges(txn.get());
                if (!status.isOK()) {
                    warning() << "Failed to enforce tag ranges" << causedBy(status);
                } else {
                    LOG(1) << "Done enforcing tag range boundaries.";
                }

                const auto candidateChunks = uassertStatusOK(
                    _chunkSelectionPolicy->selectChunksToMove(txn.get(), _balancedLastTime));

                if (candidateChunks.empty()) {
                    LOG(1) << "no need to move any chunk";
                    _balancedLastTime = false;
                } else {
                    _balancedLastTime = _moveChunks(txn.get(), candidateChunks);

                    roundDetails.setSucceeded(static_cast<int>(candidateChunks.size()),
                                              _balancedLastTime);

                    shardingContext->catalogClient(txn.get())->logAction(
                        txn.get(), "balancer.round", "", roundDetails.toBSON());
                }

                LOG(1) << "*** End of balancing round";
            }

            _endRound(txn.get(),
                      _balancedLastTime ? kShortBalanceRoundInterval
                                        : kBalanceRoundDefaultInterval);
        } catch (const std::exception& e) {
            log() << "caught exception while doing balance: " << e.what();

            // Just to match the opening statement if in log level 1
            LOG(1) << "*** End of balancing round";

            // This round failed, tell the world!
            roundDetails.setFailed(e.what());

            shardingContext->catalogClient(txn.get())->logAction(
                txn.get(), "balancer.round", "", roundDetails.toBSON());

            // Sleep a fair amount before retrying because of the error
            _endRound(txn.get(), kBalanceRoundDefaultInterval);
        }
    }

    log() << "CSRS balancer is stopped";
}

bool Balancer::_stopRequested() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    return (_state != kRunning);
}

void Balancer::_beginRound(OperationContext* txn) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _inBalancerRound = true;
    _condVar.notify_all();
}

void Balancer::_endRound(OperationContext* txn, Seconds waitTimeout) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _inBalancerRound = false;
        _numBalancerRounds++;
        _condVar.notify_all();
    }

    _sleepFor(txn, waitTimeout);
}

void Balancer::_sleepFor(OperationContext* txn, Seconds waitTimeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condVar.wait_for(lock, waitTimeout.toSystemDuration(), [&] { return _state != kRunning; });
}

bool Balancer::_checkOIDs(OperationContext* txn) {
    auto shardingContext = Grid::get(txn);

    vector<ShardId> all;
    shardingContext->shardRegistry()->getAllShardIds(&all);

    // map of OID machine ID => shardId
    map<int, ShardId> oids;

    for (const ShardId& shardId : all) {
        if (_stopRequested()) {
            return false;
        }

        const auto s = shardingContext->shardRegistry()->getShard(txn, shardId);
        if (!s) {
            continue;
        }

        auto result =
            uassertStatusOK(s->runCommand(txn,
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

                result = uassertStatusOK(
                    s->runCommand(txn,
                                  ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                  "admin",
                                  BSON("features" << 1 << "oidReset" << 1),
                                  Shard::RetryPolicy::kIdempotent));
                uassertStatusOK(result.commandStatus);

                const auto otherShard = shardingContext->shardRegistry()->getShard(txn, oids[x]);
                if (otherShard) {
                    result = uassertStatusOK(
                        otherShard->runCommand(txn,
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

Status Balancer::_enforceTagRanges(OperationContext* txn) {
    auto chunksToSplitStatus = _chunkSelectionPolicy->selectChunksToSplit(txn);
    if (!chunksToSplitStatus.isOK()) {
        return chunksToSplitStatus.getStatus();
    }

    for (const auto& splitInfo : chunksToSplitStatus.getValue()) {
        auto scopedCMStatus = ScopedChunkManager::getExisting(txn, splitInfo.nss);
        if (!scopedCMStatus.isOK()) {
            return scopedCMStatus.getStatus();
        }

        auto scopedCM = std::move(scopedCMStatus.getValue());
        ChunkManager* const cm = scopedCM.cm();

        auto splitStatus = shardutil::splitChunkAtMultiplePoints(txn,
                                                                 splitInfo.shardId,
                                                                 splitInfo.nss,
                                                                 cm->getShardKeyPattern(),
                                                                 splitInfo.collectionVersion,
                                                                 splitInfo.minKey,
                                                                 splitInfo.maxKey,
                                                                 splitInfo.splitKeys);
        if (!splitStatus.isOK()) {
            warning() << "Failed to enforce tag range for chunk " << redact(splitInfo.toString())
                      << causedBy(redact(splitStatus.getStatus()));
        }

        cm->reload(txn);
    }

    return Status::OK();
}

int Balancer::_moveChunks(OperationContext* txn,
                          const BalancerChunkSelectionPolicy::MigrateInfoVector& candidateChunks) {
    auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();

    // If the balancer was disabled since we started this round, don't start new chunk moves
    if (_stopRequested() || !balancerConfig->shouldBalance()) {
        LOG(1) << "Skipping balancing round because balancer was stopped";
        return 0;
    }

    // Schedule all migrations in parallel
    MigrationManager migrationManager;

    MigrationManager::MigrationRequestVector migrationRequests;

    for (const auto& migrateInfo : candidateChunks) {
        migrationRequests.emplace_back(migrateInfo,
                                       balancerConfig->getMaxChunkSizeBytes(),
                                       balancerConfig->getSecondaryThrottle(),
                                       balancerConfig->waitForDelete());
    }

    int numChunksProcessed = 0;

    auto migrationStatuses = migrationManager.scheduleMigrations(txn, std::move(migrationRequests));

    for (const auto& migrationStatusEntry : migrationStatuses) {
        const Status& status = migrationStatusEntry.second;
        if (status.isOK()) {
            numChunksProcessed++;
            continue;
        }

        const MigrationIdentifier& migrationId = migrationStatusEntry.first;

        if (status == ErrorCodes::ChunkTooBig) {
            numChunksProcessed++;

            auto failedRequestIt = std::find_if(candidateChunks.begin(),
                                                candidateChunks.end(),
                                                [&migrationId](const MigrateInfo& migrateInfo) {
                                                    return migrateInfo.getName() == migrationId;
                                                });
            invariant(failedRequestIt != candidateChunks.end());

            log() << "Performing a split because migration " << failedRequestIt->toString()
                  << " failed for size reasons" << causedBy(status);

            _splitOrMarkJumbo(txn, NamespaceString(failedRequestIt->ns), failedRequestIt->minKey);
            continue;
        }

        log() << "Balancer move " << migrationId << " failed" << causedBy(status);
    }

    return numChunksProcessed;
}

void Balancer::_splitOrMarkJumbo(OperationContext* txn,
                                 const NamespaceString& nss,
                                 const BSONObj& minKey) {
    auto scopedChunkManager = uassertStatusOK(ScopedChunkManager::getExisting(txn, nss));
    ChunkManager* const chunkManager = scopedChunkManager.cm();

    auto chunk = chunkManager->findIntersectingChunk(txn, minKey);

    auto splitStatus = chunk->split(txn, Chunk::normal, nullptr);
    if (!splitStatus.isOK()) {
        log() << "Marking chunk " << chunk->toString() << " as jumbo.";
        chunk->markAsJumbo(txn);
    }
}

}  // namespace mongo
