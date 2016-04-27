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

#include "mongo/s/balance.h"

#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/balancer/cluster_statistics_impl.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

using std::map;
using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {

const Seconds kBalanceRoundDefaultInterval(10);
const Seconds kShortBalanceRoundInterval(1);

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
    boost::optional<std::string> _errMsg;
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

MONGO_FP_DECLARE(skipBalanceRound);
MONGO_FP_DECLARE(balancerRoundIntervalSetting);

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

void Balancer::run() {
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

        BalanceRoundDetails roundDetails;

        try {
            // ping has to be first so we keep things in the config server in sync
            _ping(txn.get(), false);

            MONGO_FAIL_POINT_BLOCK(balancerRoundIntervalSetting, scopedBalancerRoundInterval) {
                const BSONObj& data = scopedBalancerRoundInterval.getData();
                balanceRoundInterval = Seconds(data["sleepSecs"].numberInt());
            }

            // Use fresh shard state and balancer settings
            Grid::get(txn.get())->shardRegistry()->reload(txn.get());

            auto balancerConfig = Grid::get(txn.get())->getBalancerConfiguration();
            Status refreshStatus = balancerConfig->refreshAndCheck(txn.get());
            if (!refreshStatus.isOK()) {
                warning() << "Skipping balancing round" << causedBy(refreshStatus);
                sleepFor(balanceRoundInterval);
                continue;
            }

            // now make sure we should even be running
            if (!balancerConfig->isBalancerActive() || MONGO_FAIL_POINT(skipBalanceRound)) {
                LOG(1) << "skipping balancing round because balancing is disabled";

                // Ping again so scripts can determine if we're active without waiting
                _ping(txn.get(), true);

                sleepFor(balanceRoundInterval);
                continue;
            }

            uassert(13258, "oids broken after resetting!", _checkOIDs(txn.get()));

            {
                auto scopedDistLock = grid.catalogManager(txn.get())
                                          ->distLock(txn.get(),
                                                     "balancer",
                                                     "doing balance round",
                                                     DistLockManager::kSingleLockAttemptTimeout);

                if (!scopedDistLock.isOK()) {
                    LOG(1) << "skipping balancing round" << causedBy(scopedDistLock.getStatus());

                    // Ping again so scripts can determine if we're active without waiting
                    _ping(txn.get(), true);

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

                    grid.catalogManager(txn.get())
                        ->logAction(txn.get(), "balancer.round", "", roundDetails.toBSON());
                }

                LOG(1) << "*** End of balancing round";
            }

            // Ping again so scripts can determine if we're active without waiting
            _ping(txn.get(), true);

            sleepFor(_balancedLastTime ? kShortBalanceRoundInterval : balanceRoundInterval);
        } catch (const std::exception& e) {
            log() << "caught exception while doing balance: " << e.what();

            // Just to match the opening statement if in log level 1
            LOG(1) << "*** End of balancing round";

            // This round failed, tell the world!
            roundDetails.setFailed(e.what());

            grid.catalogManager(txn.get())
                ->logAction(txn.get(), "balancer.round", "", roundDetails.toBSON());

            // Sleep a fair amount before retrying because of the error
            sleepFor(balanceRoundInterval);
        }
    }
}

bool Balancer::_init(OperationContext* txn) {
    log() << "about to contact config servers and shards";

    try {
        // Contact the config server and refresh shard information. Checks that each shard is indeed
        // a different process (no hostname mixup)
        // these checks are redundant in that they're redone at every new round but we want to do
        // them initially here so to catch any problem soon
        grid.shardRegistry()->reload(txn);
        if (!_checkOIDs(txn)) {
            return false;
        }

        log() << "config servers and shards contacted successfully";

        StringBuilder buf;
        buf << getHostNameCached() << ":" << serverGlobalParams.port;
        _myid = buf.str();

        log() << "balancer id: " << _myid << " started";

        return true;
    } catch (const std::exception& e) {
        warning() << "could not initialize balancer, please check that all shards and config "
                     "servers are up: " << e.what();
        return false;
    }
}

void Balancer::_ping(OperationContext* txn, bool waiting) {
    MongosType mType;
    mType.setName(_myid);
    mType.setPing(jsTime());
    mType.setUptime(_timer.seconds());
    mType.setWaiting(waiting);
    mType.setMongoVersion(versionString);

    grid.catalogManager(txn)->updateConfigDocument(txn,
                                                   MongosType::ConfigNS,
                                                   BSON(MongosType::name(_myid)),
                                                   BSON("$set" << mType.toBSON()),
                                                   true);
}

bool Balancer::_checkOIDs(OperationContext* txn) {
    vector<ShardId> all;
    grid.shardRegistry()->getAllShardIds(&all);

    // map of OID machine ID => shardId
    map<int, string> oids;

    for (const ShardId& shardId : all) {
        const auto s = grid.shardRegistry()->getShard(txn, shardId);
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

                const auto otherShard = grid.shardRegistry()->getShard(txn, oids[x]);
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
        // If the balancer was disabled since we started this round, don't start new chunks
        // moves.
        if (!Grid::get(txn)->getBalancerConfiguration()->isBalancerActive() ||
            MONGO_FAIL_POINT(skipBalanceRound)) {
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
            auto scopedCM = uassertStatusOK(ScopedChunkManager::getExisting(txn, nss));
            ChunkManager* const cm = scopedCM.cm();

            shared_ptr<Chunk> c = cm->findIntersectingChunk(txn, migrateInfo.minKey);

            if (c->getMin().woCompare(migrateInfo.minKey) ||
                c->getMax().woCompare(migrateInfo.maxKey)) {
                log() << "Migration " << migrateInfo
                      << " will be skipped this round due to chunk metadata mismatch.";
                scopedCM.db()->getChunkManager(txn, nss.ns(), true);
                continue;
            }

            BSONObj res;
            if (c->moveAndCommit(txn,
                                 migrateInfo.to,
                                 Grid::get(txn)->getBalancerConfiguration()->getMaxChunkSizeBytes(),
                                 secondaryThrottle,
                                 waitForDelete,
                                 0, /* maxTimeMS */
                                 res)) {
                movedCount++;
                continue;
            }

            log() << "balancer move failed: " << res << ", migrate: " << migrateInfo;

            Status moveStatus = getStatusFromCommandResult(res);

            if (moveStatus == ErrorCodes::ChunkTooBig || res["chunkTooBig"].trueValue()) {
                log() << "Performing a split because migrate failed for size reasons";

                auto splitStatus = c->split(txn, Chunk::normal, NULL);
                if (!splitStatus.isOK()) {
                    log() << "marking chunk as jumbo: " << c->toString();

                    c->markAsJumbo(txn);

                    // We increment moveCount so we do another round right away
                    movedCount++;
                }
            }
        } catch (const DBException& ex) {
            log() << "balancer move " << migrateInfo << " failed" << causedBy(ex);
        }
    }

    return movedCount;
}

}  // namespace mongo
