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

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_request.h"
#include "mongo/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/balancer/cluster_statistics_impl.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

using std::map;
using std::string;
using std::vector;

namespace {

const Seconds kBalanceRoundDefaultInterval(10);
const Seconds kShortBalanceRoundInterval(1);

const char kChunkTooBig[] = "chunkTooBig";

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

void appendOperationDeadlineIfSet(OperationContext* txn, BSONObjBuilder* cmdBuilder) {
    if (!txn->hasDeadline()) {
        return;
    }
    // Treat a remaining max time less than 1ms as 1ms, since any smaller is treated as infinity
    // or an error on the receiving node.
    const auto remainingMicros = std::max(txn->getRemainingMaxTimeMicros(), Microseconds{1000});
    const auto maxTimeMsArg = representAs<int32_t>(durationCount<Milliseconds>(remainingMicros));

    // We know that remainingMicros > 1000us, so if maxTimeMsArg is not engaged, it is because
    // remainingMicros was too big to represent as a 32-bit signed integer number of
    // milliseconds. In that case, we omit a maxTimeMs argument on the command, implying "no max
    // time".
    if (!maxTimeMsArg) {
        return;
    }
    cmdBuilder->append(QueryRequest::cmdOptionMaxTimeMS, *maxTimeMsArg);
}

/**
 * Blocking method, which requests a single chunk migration to run.
 */
Status executeSingleMigration(OperationContext* txn,
                              const MigrateInfo& migrateInfo,
                              uint64_t maxChunkSizeBytes,
                              const MigrationSecondaryThrottleOptions& secondaryThrottle,
                              bool waitForDelete) {
    const NamespaceString nss(migrateInfo.ns);

    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    ChunkManager* const cm = scopedCMStatus.getValue().cm();

    auto c = cm->findIntersectingChunk(txn, migrateInfo.minKey);

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        nss,
        cm->getVersion(),
        Grid::get(txn)->shardRegistry()->getConfigServerConnectionString(),
        migrateInfo.from,
        migrateInfo.to,
        ChunkRange(c->getMin(), c->getMax()),
        maxChunkSizeBytes,
        secondaryThrottle,
        waitForDelete,
        false);  // takeDistLock flag.

    appendOperationDeadlineIfSet(txn, &builder);

    BSONObj cmdObj = builder.obj();

    Status status{ErrorCodes::NotYetInitialized, "Uninitialized"};

    auto shard = Grid::get(txn)->shardRegistry()->getShard(txn, migrateInfo.from);
    if (!shard) {
        status = {ErrorCodes::ShardNotFound,
                  str::stream() << "shard " << migrateInfo.from << " not found"};
    } else {
        const std::string whyMessage(
            str::stream() << "migrating chunk " << ChunkRange(c->getMin(), c->getMax()).toString()
                          << " in "
                          << nss.ns());
        StatusWith<Shard::CommandResponse> cmdStatus{ErrorCodes::InternalError, "Uninitialized"};

        // Send the first moveChunk command with the balancer holding the distlock.
        {
            StatusWith<DistLockManager::ScopedDistLock> distLockStatus =
                Grid::get(txn)->catalogClient(txn)->distLock(txn, nss.ns(), whyMessage);
            if (!distLockStatus.isOK()) {
                const std::string msg = str::stream()
                    << "Could not acquire collection lock for " << nss.ns() << " to migrate chunk ["
                    << c->getMin() << "," << c->getMax() << ") due to "
                    << distLockStatus.getStatus().toString();
                warning() << msg;
                return {distLockStatus.getStatus().code(), msg};
            }

            cmdStatus = shard->runCommand(txn,
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          "admin",
                                          cmdObj,
                                          Shard::RetryPolicy::kNotIdempotent);
        }

        if (cmdStatus == ErrorCodes::LockBusy) {
            // The moveChunk source shard attempted to take the distlock despite being told not to
            // do so. The shard is likely v3.2 or earlier, which always expects to take the
            // distlock. Reattempt the moveChunk without the balancer holding the distlock so that
            // the shard can successfully acquire it.
            BSONObjBuilder builder;
            MoveChunkRequest::appendAsCommand(
                &builder,
                nss,
                cm->getVersion(),
                Grid::get(txn)->shardRegistry()->getConfigServerConnectionString(),
                migrateInfo.from,
                migrateInfo.to,
                ChunkRange(c->getMin(), c->getMax()),
                maxChunkSizeBytes,
                secondaryThrottle,
                waitForDelete,
                true);  // takeDistLock flag.

            appendOperationDeadlineIfSet(txn, &builder);

            cmdObj = builder.obj();

            cmdStatus = shard->runCommand(txn,
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          "admin",
                                          cmdObj,
                                          Shard::RetryPolicy::kIdempotent);
        }

        if (!cmdStatus.isOK()) {
            status = std::move(cmdStatus.getStatus());
        } else {
            status = std::move(cmdStatus.getValue().commandStatus);
            BSONObj cmdResponse = std::move(cmdStatus.getValue().response);

            // For backwards compatibility with 3.2 and earlier, where the move chunk command
            // instead of returning a ChunkTooBig status includes an extra field in the response
            bool chunkTooBig = false;
            bsonExtractBooleanFieldWithDefault(cmdResponse, kChunkTooBig, false, &chunkTooBig);
            if (chunkTooBig) {
                invariant(!status.isOK());
                status = {ErrorCodes::ChunkTooBig, status.reason()};
            }
        }
    }

    if (!status.isOK()) {
        log() << "Move chunk " << cmdObj << " failed" << causedBy(status);
        return {status.code(), str::stream() << "move failed due to " << status.toString()};
    }

    cm->reload(txn);

    return Status::OK();
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
        LOG(1) << "Unable to find more appropriate location for chunk " << chunk;
        return Status::OK();
    }

    auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();
    Status refreshStatus = balancerConfig->refreshAndCheck(txn);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    return executeSingleMigration(txn,
                                  *migrateInfo,
                                  balancerConfig->getMaxChunkSizeBytes(),
                                  balancerConfig->getSecondaryThrottle(),
                                  balancerConfig->waitForDelete());
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

    return executeSingleMigration(txn,
                                  MigrateInfo(chunk.getNS(), newShardId, chunk),
                                  maxChunkSizeBytes,
                                  secondaryThrottle,
                                  waitForDelete);
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
                    _balancedLastTime = 0;
                } else {
                    _balancedLastTime = _moveChunks(txn.get(),
                                                    candidateChunks,
                                                    balancerConfig->getSecondaryThrottle(),
                                                    balancerConfig->waitForDelete());

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
            warning() << "Failed to enforce tag range for chunk " << splitInfo
                      << causedBy(splitStatus.getStatus());
        }
    }

    return Status::OK();
}

int Balancer::_moveChunks(OperationContext* txn,
                          const BalancerChunkSelectionPolicy::MigrateInfoVector& candidateChunks,
                          const MigrationSecondaryThrottleOptions& secondaryThrottle,
                          bool waitForDelete) {
    int movedCount = 0;

    for (const auto& migrateInfo : candidateChunks) {
        auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();

        // If the balancer was disabled since we started this round, don't start new chunk moves
        if (_stopRequested() || !balancerConfig->shouldBalance()) {
            LOG(1) << "Stopping balancing round early as balancing was disabled";
            return movedCount;
        }

        // Changes to metadata, borked metadata, and connectivity problems between shards
        // should cause us to abort this chunk move, but shouldn't cause us to abort the entire
        // round of chunks.
        //
        // TODO(spencer): We probably *should* abort the whole round on issues communicating
        // with the config servers, but its impossible to distinguish those types of failures
        // at the moment.
        //
        // TODO: Handle all these things more cleanly, since they're expected problems

        const NamespaceString nss(migrateInfo.ns);

        try {
            Status status = executeSingleMigration(txn,
                                                   migrateInfo,
                                                   balancerConfig->getMaxChunkSizeBytes(),
                                                   balancerConfig->getSecondaryThrottle(),
                                                   balancerConfig->waitForDelete());
            if (status.isOK()) {
                movedCount++;
            } else if (status == ErrorCodes::ChunkTooBig) {
                log() << "Performing a split because migrate failed for size reasons"
                      << causedBy(status);
                _splitOrMarkJumbo(txn, nss, migrateInfo.minKey);
                movedCount++;
            } else {
                log() << "Balancer move failed" << causedBy(status);
            }
        } catch (const DBException& ex) {
            log() << "balancer move " << migrateInfo << " failed" << causedBy(ex);
        }
    }

    return movedCount;
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
