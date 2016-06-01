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
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_request.h"
#include "mongo/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/balancer/cluster_statistics_impl.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
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

const auto getBalancer = ServiceContext::declareDecoration<Balancer>();

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
        waitForDelete);
    appendOperationDeadlineIfSet(txn, &builder);

    BSONObj cmdObj = builder.obj();

    Status status{ErrorCodes::NotYetInitialized, "Uninitialized"};

    auto shard = Grid::get(txn)->shardRegistry()->getShard(txn, migrateInfo.from);
    if (!shard) {
        status = {ErrorCodes::ShardNotFound,
                  str::stream() << "shard " << migrateInfo.from << " not found"};
    } else {
        auto cmdStatus = shard->runCommand(txn,
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           "admin",
                                           cmdObj,
                                           Shard::RetryPolicy::kNotIdempotent);
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

MONGO_FP_DECLARE(skipBalanceRound);

}  // namespace

Balancer::Balancer()
    : _balancedLastTime(0),
      _chunkSelectionPolicy(stdx::make_unique<BalancerChunkSelectionPolicyImpl>(
          stdx::make_unique<ClusterStatisticsImpl>())),
      _clusterStats(stdx::make_unique<ClusterStatisticsImpl>()) {}

Balancer::~Balancer() = default;

Balancer* Balancer::get(OperationContext* operationContext) {
    return &getBalancer(operationContext->getServiceContext());
}

void Balancer::start(OperationContext* txn) {
    invariant(!_thread.joinable());
    _thread = stdx::thread([this] { _mainThread(); });
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

    _moveChunks(txn,
                {*migrateInfo},
                balancerConfig->getSecondaryThrottle(),
                balancerConfig->waitForDelete());

    return Status::OK();
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

void Balancer::_mainThread() {
    Client::initThread("Balancer");

    // This is the body of a BackgroundJob so if we throw here we're basically ending the balancer
    // thread prematurely.
    while (!inShutdown()) {
        auto txn = cc().makeOperationContext();
        if (!_init(txn.get())) {
            log() << "will retry to initialize balancer in one minute";
            sleepsecs(60);
            continue;
        }

        break;
    }

    Seconds balanceRoundInterval(kBalanceRoundDefaultInterval);

    while (!inShutdown()) {
        auto txn = cc().makeOperationContext();
        auto shardingContext = Grid::get(txn.get());
        auto balancerConfig = shardingContext->getBalancerConfiguration();

        BalanceRoundDetails roundDetails;

        try {
            // Reporting the balancer as active must be first call so the balancer control scripts
            // know that there is an active balancer
            _stardingUptimeReporter.reportStatus(txn.get(), true);

            // Use fresh shard state and balancer settings
            shardingContext->shardRegistry()->reload(txn.get());

            Status refreshStatus = balancerConfig->refreshAndCheck(txn.get());
            if (!refreshStatus.isOK()) {
                warning() << "Skipping balancing round" << causedBy(refreshStatus);
                sleepFor(balanceRoundInterval);
                continue;
            }

            // now make sure we should even be running
            if (!balancerConfig->isBalancerActive() || MONGO_FAIL_POINT(skipBalanceRound)) {
                LOG(1) << "skipping balancing round because balancing is disabled";

                // Tell scripts that the balancer is not active anymore
                _stardingUptimeReporter.reportStatus(txn.get(), false);

                sleepFor(balanceRoundInterval);
                continue;
            }

            uassert(13258, "oids broken after resetting!", _checkOIDs(txn.get()));

            {
                auto scopedDistLock = shardingContext->catalogManager(txn.get())->distLock(
                    txn.get(),
                    "balancer",
                    "doing balance round",
                    DistLockManager::kSingleLockAttemptTimeout);

                if (!scopedDistLock.isOK()) {
                    LOG(1) << "skipping balancing round" << causedBy(scopedDistLock.getStatus());

                    // Tell scripts that the balancer is not active anymore
                    _stardingUptimeReporter.reportStatus(txn.get(), false);

                    sleepFor(balanceRoundInterval);  // no need to wake up soon
                    continue;
                }

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

                    shardingContext->catalogManager(txn.get())->logAction(
                        txn.get(), "balancer.round", "", roundDetails.toBSON());
                }

                LOG(1) << "*** End of balancing round";
            }

            // Tell scripts that the balancer is not active anymore
            _stardingUptimeReporter.reportStatus(txn.get(), false);

            sleepFor(_balancedLastTime ? kShortBalanceRoundInterval : balanceRoundInterval);
        } catch (const std::exception& e) {
            log() << "caught exception while doing balance: " << e.what();

            // Just to match the opening statement if in log level 1
            LOG(1) << "*** End of balancing round";

            // This round failed, tell the world!
            roundDetails.setFailed(e.what());

            shardingContext->catalogManager(txn.get())->logAction(
                txn.get(), "balancer.round", "", roundDetails.toBSON());

            // Sleep a fair amount before retrying because of the error
            sleepFor(balanceRoundInterval);
        }
    }
}

bool Balancer::_init(OperationContext* txn) {
    log() << "about to contact config servers and shards";

    try {
        // Contact the config server and refresh shard information. Checks that each shard is indeed
        // a different process (no hostname mixup).
        //
        // These checks are redundant in that they're redone at every new round but we want to do
        // them initially here so to catch any problem soon.
        Grid::get(txn)->shardRegistry()->reload(txn);

        if (!_checkOIDs(txn)) {
            return false;
        }

        log() << "config servers and shards contacted successfully";
        _stardingUptimeReporter.reportStatus(txn, false);

        log() << "balancer id: " << _stardingUptimeReporter.getInstanceId() << " started";

        return true;
    } catch (const std::exception& e) {
        warning() << "could not initialize balancer, please check that all shards and config "
                     "servers are up: "
                  << e.what();
        return false;
    }
}

bool Balancer::_checkOIDs(OperationContext* txn) {
    auto shardingContext = Grid::get(txn);

    vector<ShardId> all;
    shardingContext->shardRegistry()->getAllShardIds(&all);

    // map of OID machine ID => shardId
    map<int, string> oids;

    for (const ShardId& shardId : all) {
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
                                                                 {splitInfo.splitKey});
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
        if (!balancerConfig->isBalancerActive()) {
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

                auto scopedCM = uassertStatusOK(ScopedChunkManager::getExisting(txn, nss));
                ChunkManager* const cm = scopedCM.cm();

                auto c = cm->findIntersectingChunk(txn, migrateInfo.minKey);

                auto splitStatus = c->split(txn, Chunk::normal, nullptr);
                if (!splitStatus.isOK()) {
                    log() << "Marking chunk " << c->toString() << " as jumbo.";

                    c->markAsJumbo(txn);

                    // We increment moveCount so we do another round right away
                    movedCount++;
                }
            } else {
                log() << "Balancer move failed" << causedBy(status);
            }
        } catch (const DBException& ex) {
            log() << "balancer move " << migrateInfo << " failed" << causedBy(ex);
        }
    }

    return movedCount;
}

}  // namespace mongo
