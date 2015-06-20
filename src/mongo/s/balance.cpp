/**
*    Copyright (C) 2008 10gen Inc.
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

#include <algorithm>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer_policy.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_actionlog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/type_mongos.h"
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

MONGO_FP_DECLARE(skipBalanceRound);

Balancer balancer;

Balancer::Balancer() : _balancedLastTime(0), _policy(new BalancerPolicy()) {}

Balancer::~Balancer() = default;

int Balancer::_moveChunks(const vector<shared_ptr<MigrateInfo>>& candidateChunks,
                          const WriteConcernOptions* writeConcern,
                          bool waitForDelete) {
    int movedCount = 0;

    for (const auto& migrateInfo : candidateChunks) {
        // If the balancer was disabled since we started this round, don't start new chunks
        // moves.
        const auto balSettingsResult =
            grid.catalogManager()->getGlobalSettings(SettingsType::BalancerDocKey);

        const bool isBalSettingsAbsent =
            balSettingsResult.getStatus() == ErrorCodes::NoMatchingDocument;

        if (!balSettingsResult.isOK() && !isBalSettingsAbsent) {
            warning() << balSettingsResult.getStatus();
            return movedCount;
        }

        const SettingsType& balancerConfig =
            isBalSettingsAbsent ? SettingsType{} : balSettingsResult.getValue();

        if ((!isBalSettingsAbsent && !grid.shouldBalance(balancerConfig)) ||
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

        const NamespaceString nss(migrateInfo->ns);

        try {
            auto status = grid.catalogCache()->getDatabase(nss.db().toString());
            fassert(28628, status.getStatus());

            shared_ptr<DBConfig> cfg = status.getValue();

            // NOTE: We purposely do not reload metadata here, since _doBalanceRound already
            // tried to do so once.
            shared_ptr<ChunkManager> cm = cfg->getChunkManager(migrateInfo->ns);
            invariant(cm);

            ChunkPtr c = cm->findIntersectingChunk(migrateInfo->chunk.min);

            if (c->getMin().woCompare(migrateInfo->chunk.min) ||
                c->getMax().woCompare(migrateInfo->chunk.max)) {
                // Likely a split happened somewhere, so force reload the chunk manager
                cm = cfg->getChunkManager(migrateInfo->ns, true);
                invariant(cm);

                c = cm->findIntersectingChunk(migrateInfo->chunk.min);

                if (c->getMin().woCompare(migrateInfo->chunk.min) ||
                    c->getMax().woCompare(migrateInfo->chunk.max)) {
                    log() << "chunk mismatch after reload, ignoring will retry issue "
                          << migrateInfo->chunk.toString();

                    continue;
                }
            }

            BSONObj res;
            if (c->moveAndCommit(migrateInfo->to,
                                 Chunk::MaxChunkSize,
                                 writeConcern,
                                 waitForDelete,
                                 0, /* maxTimeMS */
                                 res)) {
                movedCount++;
                continue;
            }

            // The move requires acquiring the collection metadata's lock, which can fail.
            log() << "balancer move failed: " << res << " from: " << migrateInfo->from
                  << " to: " << migrateInfo->to << " chunk: " << migrateInfo->chunk;

            if (res["chunkTooBig"].trueValue()) {
                // Reload just to be safe
                cm = cfg->getChunkManager(migrateInfo->ns);
                invariant(cm);

                c = cm->findIntersectingChunk(migrateInfo->chunk.min);

                log() << "performing a split because migrate failed for size reasons";

                Status status = c->split(Chunk::normal, NULL, NULL);
                log() << "split results: " << status;

                if (!status.isOK()) {
                    log() << "marking chunk as jumbo: " << c->toString();

                    c->markAsJumbo();

                    // We increment moveCount so we do another round right away
                    movedCount++;
                }
            }
        } catch (const DBException& ex) {
            warning() << "could not move chunk " << migrateInfo->chunk.toString()
                      << ", continuing balancing round" << causedBy(ex);
        }
    }

    return movedCount;
}

void Balancer::_ping(bool waiting) {
    grid.catalogManager()->update(
        MongosType::ConfigNS,
        BSON(MongosType::name(_myid)),
        BSON("$set" << BSON(MongosType::ping(jsTime())
                            << MongosType::up(static_cast<int>(time(0) - _started))
                            << MongosType::waiting(waiting)
                            << MongosType::mongoVersion(versionString))),
        true,
        false,
        NULL);
}

/*
* Builds the details object for the actionlog.
* Current formats for detail are:
* Success: {
*           "candidateChunks" : ,
*           "chunksMoved" : ,
*           "executionTimeMillis" : ,
*           "errorOccured" : false
*          }
* Failure: {
*           "executionTimeMillis" : ,
*           "errmsg" : ,
*           "errorOccured" : true
*          }
* @param didError, did this round end in an error?
* @param executionTime, the time this round took to run
* @param candidateChunks, the number of chunks identified to be moved
* @param chunksMoved, the number of chunks moved
* @param errmsg, the error message for this round
*/

static BSONObj _buildDetails(bool didError,
                             int executionTime,
                             int candidateChunks,
                             int chunksMoved,
                             const std::string& errmsg) {
    BSONObjBuilder builder;
    builder.append("executionTimeMillis", executionTime);
    builder.append("errorOccured", didError);

    if (didError) {
        builder.append("errmsg", errmsg);
    } else {
        builder.append("candidateChunks", candidateChunks);
        builder.append("chunksMoved", chunksMoved);
    }
    return builder.obj();
}

bool Balancer::_checkOIDs() {
    vector<ShardId> all;
    grid.shardRegistry()->getAllShardIds(&all);

    // map of OID machine ID => shardId
    map<int, string> oids;

    for (const ShardId& shardId : all) {
        const auto s = grid.shardRegistry()->getShard(shardId);
        if (!s) {
            continue;
        }

        BSONObj f = s->runCommand("admin", "features");
        if (f["oidMachine"].isNumber()) {
            int x = f["oidMachine"].numberInt();
            if (oids.count(x) == 0) {
                oids[x] = shardId;
            } else {
                log() << "error: 2 machines have " << x << " as oid machine piece: " << shardId
                      << " and " << oids[x];
                s->runCommand("admin", BSON("features" << 1 << "oidReset" << 1));

                const auto otherShard = grid.shardRegistry()->getShard(oids[x]);
                if (otherShard) {
                    otherShard->runCommand("admin", BSON("features" << 1 << "oidReset" << 1));
                }
                return false;
            }
        } else {
            log() << "warning: oidMachine not set on: " << s->toString();
        }
    }
    return true;
}

/**
 * Occasionally prints a log message with shard versions if the versions are not the same
 * in the cluster.
 */
void warnOnMultiVersion(const ShardInfoMap& shardInfo) {
    bool isMultiVersion = false;
    for (ShardInfoMap::const_iterator i = shardInfo.begin(); i != shardInfo.end(); ++i) {
        if (!isSameMajorVersion(i->second.getMongoVersion().c_str())) {
            isMultiVersion = true;
            break;
        }
    }

    // If we're all the same version, don't message
    if (!isMultiVersion)
        return;

    warning() << "multiVersion cluster detected, my version is " << versionString;
    for (ShardInfoMap::const_iterator i = shardInfo.begin(); i != shardInfo.end(); ++i) {
        log() << i->first << " is at version " << i->second.getMongoVersion();
    }
}

void Balancer::_doBalanceRound(vector<shared_ptr<MigrateInfo>>* candidateChunks) {
    invariant(candidateChunks);

    vector<CollectionType> collections;
    Status collsStatus = grid.catalogManager()->getCollections(nullptr, &collections);
    if (!collsStatus.isOK()) {
        warning() << "Failed to retrieve the set of collections during balancing round "
                  << collsStatus;
        return;
    }

    if (collections.empty()) {
        LOG(1) << "no collections to balance";
        return;
    }

    // Get a list of all the shards that are participating in this balance round along with any
    // maximum allowed quotas and current utilization. We get the latter by issuing
    // db.serverStatus() (mem.mapped) to all shards.
    //
    // TODO: skip unresponsive shards and mark information as stale.
    ShardInfoMap shardInfo;
    Status loadStatus = DistributionStatus::populateShardInfoMap(&shardInfo);
    if (!loadStatus.isOK()) {
        warning() << "failed to load shard metadata" << causedBy(loadStatus);
        return;
    }

    if (shardInfo.size() < 2) {
        LOG(1) << "can't balance without more active shards";
        return;
    }

    OCCASIONALLY warnOnMultiVersion(shardInfo);

    // For each collection, check if the balancing policy recommends moving anything around.
    for (const auto& coll : collections) {
        // Skip collections for which balancing is disabled
        const NamespaceString& ns = coll.getNs();

        if (!coll.getAllowBalance()) {
            LOG(1) << "Not balancing collection " << ns << "; explicitly disabled.";
            continue;
        }

        std::vector<ChunkType> allNsChunks;
        grid.catalogManager()->getChunks(Query(BSON(ChunkType::ns(ns))).sort(ChunkType::min()),
                                         0,  // all chunks
                                         &allNsChunks);

        set<BSONObj> allChunkMinimums;
        map<string, vector<ChunkType>> shardToChunksMap;

        for (const ChunkType& chunk : allNsChunks) {
            allChunkMinimums.insert(chunk.getMin().getOwned());

            vector<ChunkType>& chunksList = shardToChunksMap[chunk.getShard()];
            chunksList.push_back(chunk);
        }

        if (shardToChunksMap.empty()) {
            LOG(1) << "skipping empty collection (" << ns << ")";
            continue;
        }

        for (ShardInfoMap::const_iterator i = shardInfo.begin(); i != shardInfo.end(); ++i) {
            // This loop just makes sure there is an entry in shardToChunksMap for every shard
            shardToChunksMap[i->first];
        }

        DistributionStatus status(shardInfo, shardToChunksMap);

        // TODO: TagRange contains all the information from TagsType except for the namespace,
        //       so maybe the two can be merged at some point in order to avoid the
        //       transformation below.
        vector<TagRange> ranges;

        {
            vector<TagsType> collectionTags;
            uassertStatusOK(
                grid.catalogManager()->getTagsForCollection(ns.toString(), &collectionTags));
            for (const auto& tt : collectionTags) {
                ranges.push_back(
                    TagRange(tt.getMinKey().getOwned(), tt.getMaxKey().getOwned(), tt.getTag()));
                uassert(16356,
                        str::stream() << "tag ranges not valid for: " << ns.toString(),
                        status.addTagRange(ranges.back()));
            }
        }

        auto statusGetDb = grid.catalogCache()->getDatabase(ns.db().toString());
        if (!statusGetDb.isOK()) {
            warning() << "could not load db config to balance collection [" << ns
                      << "]: " << statusGetDb.getStatus();
            continue;
        }

        shared_ptr<DBConfig> cfg = statusGetDb.getValue();

        // This line reloads the chunk manager once if this process doesn't know the collection
        // is sharded yet.
        shared_ptr<ChunkManager> cm = cfg->getChunkManagerIfExists(ns, true);
        if (!cm) {
            warning() << "could not load chunks to balance " << ns << " collection";
            continue;
        }

        // Loop through tags to make sure no chunk spans tags. Split on tag min for all chunks.
        bool didAnySplits = false;

        for (const TagRange& range : ranges) {
            BSONObj min =
                cm->getShardKeyPattern().getKeyPattern().extendRangeBound(range.min, false);

            if (allChunkMinimums.count(min) > 0) {
                continue;
            }

            didAnySplits = true;

            log() << "ns: " << ns << " need to split on " << min
                  << " because there is a range there";

            ChunkPtr c = cm->findIntersectingChunk(min);

            vector<BSONObj> splitPoints;
            splitPoints.push_back(min);

            Status status = c->multiSplit(splitPoints, NULL);
            if (!status.isOK()) {
                error() << "split failed: " << status;
            } else {
                LOG(1) << "split worked";
            }

            break;
        }

        if (didAnySplits) {
            // State change, just wait till next round
            continue;
        }

        shared_ptr<MigrateInfo> migrateInfo(_policy->balance(ns, status, _balancedLastTime));
        if (migrateInfo) {
            candidateChunks->push_back(migrateInfo);
        }
    }
}

bool Balancer::_init() {
    try {
        log() << "about to contact config servers and shards";

        // contact the config server and refresh shard information
        // checks that each shard is indeed a different process (no hostname mixup)
        // these checks are redundant in that they're redone at every new round but we want to do them initially here
        // so to catch any problem soon
        Shard::reloadShardInfo();
        _checkOIDs();

        log() << "config servers and shards contacted successfully";

        StringBuilder buf;
        buf << getHostNameCached() << ":" << serverGlobalParams.port;
        _myid = buf.str();
        _started = time(0);

        log() << "balancer id: " << _myid << " started";

        return true;

    } catch (std::exception& e) {
        warning() << "could not initialize balancer, please check that all shards and config "
                     "servers are up: " << e.what();
        return false;
    }
}

void Balancer::run() {
    Client::initThread("Balancer");

    // This is the body of a BackgroundJob so if we throw here we're basically ending the
    // balancer thread prematurely.
    while (!inShutdown()) {
        if (!_init()) {
            log() << "will retry to initialize balancer in one minute";
            sleepsecs(60);
            continue;
        }

        break;
    }

    const int sleepTime = 10;

    while (!inShutdown()) {
        Timer balanceRoundTimer;
        ActionLogType actionLog;

        actionLog.setServer(getHostNameCached());
        actionLog.setWhat("balancer.round");

        try {
            // ping has to be first so we keep things in the config server in sync
            _ping();

            BSONObj balancerResult;

            // use fresh shard state
            Shard::reloadShardInfo();

            // refresh chunk size (even though another balancer might be active)
            Chunk::refreshChunkSize();

            auto balSettingsResult =
                grid.catalogManager()->getGlobalSettings(SettingsType::BalancerDocKey);
            const bool isBalSettingsAbsent =
                balSettingsResult.getStatus() == ErrorCodes::NoMatchingDocument;
            if (!balSettingsResult.isOK() && !isBalSettingsAbsent) {
                warning() << balSettingsResult.getStatus();
                return;
            }
            const SettingsType& balancerConfig =
                isBalSettingsAbsent ? SettingsType{} : balSettingsResult.getValue();

            // now make sure we should even be running
            if ((!isBalSettingsAbsent && !grid.shouldBalance(balancerConfig)) ||
                MONGO_FAIL_POINT(skipBalanceRound)) {
                LOG(1) << "skipping balancing round because balancing is disabled";

                // Ping again so scripts can determine if we're active without waiting
                _ping(true);

                sleepsecs(sleepTime);
                continue;
            }

            uassert(13258, "oids broken after resetting!", _checkOIDs());

            {
                auto scopedDistLock = grid.catalogManager()->getDistLockManager()->lock(
                    "balancer", "doing balance round");

                if (!scopedDistLock.isOK()) {
                    LOG(1) << "skipping balancing round" << causedBy(scopedDistLock.getStatus());

                    // Ping again so scripts can determine if we're active without waiting
                    _ping(true);

                    sleepsecs(sleepTime);  // no need to wake up soon
                    continue;
                }

                const bool waitForDelete =
                    (balancerConfig.isWaitForDeleteSet() ? balancerConfig.getWaitForDelete()
                                                         : false);

                std::unique_ptr<WriteConcernOptions> writeConcern;
                if (balancerConfig.isKeySet()) {  // if balancer doc exists.
                    writeConcern = std::move(balancerConfig.getWriteConcern());
                }

                LOG(1) << "*** start balancing round. "
                       << "waitForDelete: " << waitForDelete << ", secondaryThrottle: "
                       << (writeConcern.get() ? writeConcern->toBSON().toString() : "default");

                vector<shared_ptr<MigrateInfo>> candidateChunks;
                _doBalanceRound(&candidateChunks);

                if (candidateChunks.size() == 0) {
                    LOG(1) << "no need to move any chunk";
                    _balancedLastTime = 0;
                } else {
                    _balancedLastTime =
                        _moveChunks(candidateChunks, writeConcern.get(), waitForDelete);
                }

                actionLog.setDetails(_buildDetails(false,
                                                   balanceRoundTimer.millis(),
                                                   static_cast<int>(candidateChunks.size()),
                                                   _balancedLastTime,
                                                   ""));
                actionLog.setTime(jsTime());

                grid.catalogManager()->logAction(actionLog);

                LOG(1) << "*** end of balancing round";
            }

            // Ping again so scripts can determine if we're active without waiting
            _ping(true);

            sleepsecs(_balancedLastTime ? sleepTime / 10 : sleepTime);
        } catch (std::exception& e) {
            log() << "caught exception while doing balance: " << e.what();

            // Just to match the opening statement if in log level 1
            LOG(1) << "*** End of balancing round";

            // This round failed, tell the world!
            actionLog.setDetails(_buildDetails(true, balanceRoundTimer.millis(), 0, 0, e.what()));
            actionLog.setTime(jsTime());

            grid.catalogManager()->logAction(actionLog);

            // Sleep a fair amount before retrying because of the error
            sleepsecs(sleepTime);

            continue;
        }
    }
}

}  // namespace mongo
