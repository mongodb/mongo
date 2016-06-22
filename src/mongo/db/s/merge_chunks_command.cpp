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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::shared_ptr;
using std::vector;

namespace {

BSONArray buildOpPrecond(const string& ns,
                         const ShardId& shardName,
                         const ChunkVersion& shardVersion) {
    BSONArrayBuilder preCond;
    BSONObjBuilder condB;
    condB.append("ns", ChunkType::ConfigNS);
    condB.append("q",
                 BSON("query" << BSON(ChunkType::ns(ns)) << "orderby"
                              << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
    {
        BSONObjBuilder resB(condB.subobjStart("res"));
        shardVersion.addToBSON(resB, ChunkType::DEPRECATED_lastmod());
        resB.done();
    }
    preCond.append(condB.obj());
    return preCond.arr();
}

BSONObj buildOpMergeChunk(const ChunkType& mergedChunk) {
    BSONObjBuilder opB;

    // Op basics
    opB.append("op", "u");
    opB.appendBool("b", false);  // no upserting
    opB.append("ns", ChunkType::ConfigNS);

    // New object
    opB.append("o", mergedChunk.toBSON());

    // Query object
    opB.append("o2", BSON(ChunkType::name(mergedChunk.getName())));

    return opB.obj();
}

BSONObj buildOpRemoveChunk(const ChunkType& chunkToRemove) {
    BSONObjBuilder opB;

    // Op basics
    opB.append("op", "d");  // delete
    opB.append("ns", ChunkType::ConfigNS);

    opB.append("o", BSON(ChunkType::name(chunkToRemove.getName())));

    return opB.obj();
}

BSONObj buildMergeLogEntry(const std::vector<ChunkType>& chunksToMerge,
                           const ChunkVersion& currShardVersion,
                           const ChunkVersion& newMergedVersion) {
    BSONObjBuilder logDetailB;

    BSONArrayBuilder mergedB(logDetailB.subarrayStart("merged"));

    for (const ChunkType& chunkToMerge : chunksToMerge) {
        mergedB.append(chunkToMerge.toBSON());
    }

    mergedB.done();

    currShardVersion.addToBSON(logDetailB, "prevShardVersion");
    newMergedVersion.addToBSON(logDetailB, "mergedVersion");

    return logDetailB.obj();
}

Status runApplyOpsCmd(OperationContext* txn,
                      const std::vector<ChunkType>& chunksToMerge,
                      const ChunkVersion& currShardVersion,
                      const ChunkVersion& newMergedVersion) {
    BSONArrayBuilder updatesB;

    // The chunk we'll be "expanding" is the first chunk
    const ChunkType& firstChunk = chunksToMerge.front();

    // Fill in details not tracked by metadata
    ChunkType mergedChunk(firstChunk);
    mergedChunk.setMax(chunksToMerge.back().getMax());
    mergedChunk.setVersion(newMergedVersion);

    updatesB.append(buildOpMergeChunk(mergedChunk));

    // Don't remove chunk we're expanding
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        ChunkType chunkToMerge(chunksToMerge[i]);
        updatesB.append(buildOpRemoveChunk(chunkToMerge));
    }

    BSONArray preCond = buildOpPrecond(firstChunk.getNS(), firstChunk.getShard(), currShardVersion);

    return grid.catalogClient(txn)->applyChunkOpsDeprecated(
        txn, updatesB.arr(), preCond, firstChunk.getNS(), newMergedVersion);
}

bool mergeChunks(OperationContext* txn,
                 const NamespaceString& nss,
                 const BSONObj& minKey,
                 const BSONObj& maxKey,
                 const OID& epoch,
                 string* errMsg) {
    // Get the distributed lock
    const string whyMessage = stream() << "merging chunks in " << nss.ns() << " from " << minKey
                                       << " to " << maxKey;
    auto scopedDistLock = grid.catalogClient(txn)->distLock(
        txn, nss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);

    if (!scopedDistLock.isOK()) {
        *errMsg = stream() << "could not acquire collection lock for " << nss.ns()
                           << " to merge chunks in [" << minKey << "," << maxKey << ")"
                           << causedBy(scopedDistLock.getStatus());

        warning() << *errMsg;
        return false;
    }

    ShardingState* gss = ShardingState::get(txn);

    //
    // We now have the collection lock, refresh metadata to latest version and sanity check
    //

    ChunkVersion shardVersion;
    Status status = gss->refreshMetadataNow(txn, nss.ns(), &shardVersion);

    if (!status.isOK()) {
        *errMsg = str::stream() << "could not merge chunks, failed to refresh metadata for "
                                << nss.ns() << causedBy(status.reason());

        warning() << *errMsg;
        return false;
    }

    if (epoch.isSet() && shardVersion.epoch() != epoch) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns() << " has changed"
                           << " since merge was sent"
                           << "(sent epoch : " << epoch.toString()
                           << ", current epoch : " << shardVersion.epoch().toString() << ")";

        warning() << *errMsg;
        return false;
    }

    ScopedCollectionMetadata metadata;
    {
        AutoGetCollection autoColl(txn, nss, MODE_IS);

        metadata = CollectionShardingState::get(txn, nss.ns())->getMetadata();
        if (!metadata || metadata->getKeyPattern().isEmpty()) {
            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " is not sharded";

            warning() << *errMsg;
            return false;
        }
    }

    dassert(metadata->getShardVersion().equals(shardVersion));

    if (!metadata->isValidKey(minKey) || !metadata->isValidKey(maxKey)) {
        *errMsg = stream() << "could not merge chunks, the range " << rangeToString(minKey, maxKey)
                           << " is not valid"
                           << " for collection " << nss.ns() << " with key pattern "
                           << metadata->getKeyPattern();

        warning() << *errMsg;
        return false;
    }


    //
    // Get merged chunk information
    //

    std::vector<ChunkType> chunksToMerge;

    ChunkType itChunk;
    itChunk.setMin(minKey);
    itChunk.setMax(minKey);
    itChunk.setNS(nss.ns());
    itChunk.setShard(gss->getShardName());

    while (itChunk.getMax().woCompare(maxKey) < 0 &&
           metadata->getNextChunk(itChunk.getMax(), &itChunk)) {
        chunksToMerge.push_back(itChunk);
    }


    if (chunksToMerge.empty()) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " range starting at " << minKey << " and ending at " << maxKey
                           << " does not belong to shard " << gss->getShardName();

        warning() << *errMsg;
        return false;
    }

    //
    // Validate the range starts and ends at chunks and has no holes, error if not valid
    //

    BSONObj firstDocMin = chunksToMerge.front().getMin();
    BSONObj firstDocMax = chunksToMerge.front().getMax();
    // minKey is inclusive
    bool minKeyInRange = rangeContains(firstDocMin, firstDocMax, minKey);

    if (!minKeyInRange) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " range starting at " << minKey << " does not belong to shard "
                           << gss->getShardName();

        warning() << *errMsg;
        return false;
    }

    BSONObj lastDocMin = chunksToMerge.back().getMin();
    BSONObj lastDocMax = chunksToMerge.back().getMax();
    // maxKey is exclusive
    bool maxKeyInRange = lastDocMin.woCompare(maxKey) < 0 && lastDocMax.woCompare(maxKey) >= 0;

    if (!maxKeyInRange) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " range ending at " << maxKey << " does not belong to shard "
                           << gss->getShardName();

        warning() << *errMsg;
        return false;
    }

    bool validRangeStartKey = firstDocMin.woCompare(minKey) == 0;
    bool validRangeEndKey = lastDocMax.woCompare(maxKey) == 0;

    if (!validRangeStartKey || !validRangeEndKey) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " does not contain a chunk "
                           << (!validRangeStartKey ? "starting at " + minKey.toString() : "")
                           << (!validRangeStartKey && !validRangeEndKey ? " or " : "")
                           << (!validRangeEndKey ? "ending at " + maxKey.toString() : "");

        warning() << *errMsg;
        return false;
    }

    if (chunksToMerge.size() == 1) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " already contains chunk for " << rangeToString(minKey, maxKey);

        warning() << *errMsg;
        return false;
    }

    // Look for hole in range
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        if (chunksToMerge[i - 1].getMax().woCompare(chunksToMerge[i].getMin()) != 0) {
            *errMsg =
                stream() << "could not merge chunks, collection " << nss.ns()
                         << " has a hole in the range " << rangeToString(minKey, maxKey) << " at "
                         << rangeToString(chunksToMerge[i - 1].getMax(), chunksToMerge[i].getMin());

            warning() << *errMsg;
            return false;
        }
    }

    //
    // Run apply ops command
    //
    ChunkVersion mergeVersion = metadata->getCollVersion();
    mergeVersion.incMinor();

    Status applyOpsStatus = runApplyOpsCmd(txn, chunksToMerge, shardVersion, mergeVersion);
    if (!applyOpsStatus.isOK()) {
        warning() << applyOpsStatus;
        return false;
    }

    //
    // Install merged chunk metadata
    //

    {
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, nss, MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, nss);

        std::unique_ptr<CollectionMetadata> cloned(
            uassertStatusOK(css->getMetadata()->cloneMerge(minKey, maxKey, mergeVersion)));
        css->setMetadata(std::move(cloned));
    }

    //
    // Log change
    //

    BSONObj mergeLogEntry = buildMergeLogEntry(chunksToMerge, shardVersion, mergeVersion);

    grid.catalogClient(txn)->logChange(txn, "merge", nss.ns(), mergeLogEntry);

    return true;
}

class MergeChunksCommand : public Command {
public:
    MergeChunksCommand() : Command("mergeChunks") {}

    void help(stringstream& h) const override {
        h << "Merge Chunks command\n"
          << "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ],"
          << " (opt) epoch : <epoch>, (opt) config : <configdb string>,"
          << " (opt) shardName : <shard name> }";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool adminOnly() const override {
        return true;
    }

    bool slaveOk() const override {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    // Required
    static BSONField<string> nsField;
    static BSONField<vector<BSONObj>> boundsField;
    // Optional, if the merge is only valid for a particular epoch
    static BSONField<OID> epochField;
    // Optional, if our sharding state has not previously been initializeed
    static BSONField<string> shardNameField;
    static BSONField<string> configField;

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) override {
        string ns = parseNs(dbname, cmdObj);

        if (ns.size() == 0) {
            errmsg = "no namespace specified";
            return false;
        }

        vector<BSONObj> bounds;
        if (!FieldParser::extract(cmdObj, boundsField, &bounds, &errmsg)) {
            return false;
        }

        if (bounds.size() == 0) {
            errmsg = "no bounds were specified";
            return false;
        }

        if (bounds.size() != 2) {
            errmsg = "only a min and max bound may be specified";
            return false;
        }

        BSONObj minKey = bounds[0];
        BSONObj maxKey = bounds[1];

        if (minKey.isEmpty()) {
            errmsg = "no min key specified";
            return false;
        }

        if (maxKey.isEmpty()) {
            errmsg = "no max key specified";
            return false;
        }

        //
        // This might be the first call from mongos, so we may need to pass the config and shard
        // information to initialize the sharding state.
        //

        ShardingState* gss = ShardingState::get(txn);

        string config;
        FieldParser::FieldState extracted =
            FieldParser::extract(cmdObj, configField, &config, &errmsg);
        if (!gss->enabled()) {
            if (!extracted || extracted == FieldParser::FIELD_NONE) {
                errmsg =
                    "sharding state must be enabled or "
                    "config server specified to merge chunks";
                return false;
            }

            gss->initializeFromConfigConnString(txn, config);
        }

        // ShardName is optional, but might not be set yet
        string shardName;
        extracted = FieldParser::extract(cmdObj, shardNameField, &shardName, &errmsg);

        if (!extracted)
            return false;
        if (extracted != FieldParser::FIELD_NONE) {
            gss->setShardName(shardName);
        }

        //
        // Epoch is optional, and if not set indicates we should use the latest epoch
        //

        OID epoch;
        if (!FieldParser::extract(cmdObj, epochField, &epoch, &errmsg)) {
            return false;
        }

        return mergeChunks(txn, NamespaceString(ns), minKey, maxKey, epoch, &errmsg);
    }

} mergeChunksCmd;

BSONField<string> MergeChunksCommand::nsField("mergeChunks");
BSONField<vector<BSONObj>> MergeChunksCommand::boundsField("bounds");

BSONField<string> MergeChunksCommand::configField("config");
BSONField<string> MergeChunksCommand::shardNameField("shardName");
BSONField<OID> MergeChunksCommand::epochField("epoch");

}  // namespace
}  // namespace mongo
