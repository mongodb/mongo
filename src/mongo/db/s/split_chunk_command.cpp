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

#include <string>
#include <vector>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

namespace {

/**
 * Append min, max and version information from chunk to the buffer.
 */
void appendShortVersion(BufBuilder& b, const ChunkType& chunk) {
    BSONObjBuilder bb(b);
    bb.append(ChunkType::min(), chunk.getMin());
    bb.append(ChunkType::max(), chunk.getMax());
    if (chunk.isVersionSet())
        chunk.getVersion().addToBSON(bb, ChunkType::DEPRECATED_lastmod());
    bb.done();
}

bool checkIfSingleDoc(OperationContext* txn,
                      Collection* collection,
                      const IndexDescriptor* idx,
                      const ChunkType* chunk) {
    KeyPattern kp(idx->keyPattern());
    BSONObj newmin = Helpers::toKeyFormat(kp.extendRangeBound(chunk->getMin(), false));
    BSONObj newmax = Helpers::toKeyFormat(kp.extendRangeBound(chunk->getMax(), true));

    unique_ptr<PlanExecutor> exec(InternalPlanner::indexScan(txn,
                                                             collection,
                                                             idx,
                                                             newmin,
                                                             newmax,
                                                             false,  // endKeyInclusive
                                                             PlanExecutor::YIELD_MANUAL));
    // check if exactly one document found
    PlanExecutor::ExecState state;
    BSONObj obj;
    if (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
        if (PlanExecutor::IS_EOF == (state = exec->getNext(&obj, NULL))) {
            return true;
        }
    }

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    return false;
}

class SplitChunkCommand : public Command {
public:
    SplitChunkCommand() : Command("splitChunk") {}

    void help(std::stringstream& help) const override {
        help << "internal command usage only\n"
                "example:\n"
                " { splitChunk:\"db.foo\" , keyPattern: {a:1} , min : {a:100} , max: {a:200} { "
                "splitKeys : [ {a:150} , ... ]}";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
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

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        //
        // 1. check whether parameters passed to splitChunk are sound
        //

        const NamespaceString nss = NamespaceString(parseNs(dbname, cmdObj));
        if (!nss.isValid()) {
            errmsg = str::stream() << "invalid namespace '" << nss.toString()
                                   << "' specified for command";
            return false;
        }

        const BSONObj keyPattern = cmdObj["keyPattern"].Obj();
        if (keyPattern.isEmpty()) {
            errmsg = "need to specify the key pattern the collection is sharded over";
            return false;
        }

        const BSONObj min = cmdObj["min"].Obj();
        if (min.isEmpty()) {
            errmsg = "need to specify the min key for the chunk";
            return false;
        }

        const BSONObj max = cmdObj["max"].Obj();
        if (max.isEmpty()) {
            errmsg = "need to specify the max key for the chunk";
            return false;
        }

        const string shardName = cmdObj["from"].str();
        if (shardName.empty()) {
            errmsg = "need specify server to split chunk at";
            return false;
        }

        const BSONObj splitKeysElem = cmdObj["splitKeys"].Obj();
        if (splitKeysElem.isEmpty()) {
            errmsg = "need to provide the split points to chunk over";
            return false;
        }
        vector<BSONObj> splitKeys;
        BSONObjIterator it(splitKeysElem);
        while (it.more()) {
            splitKeys.push_back(it.next().Obj().getOwned());
        }

        //
        // Get sharding state up-to-date
        //

        ShardingState* const shardingState = ShardingState::get(txn);

        // This could be the first call that enables sharding - make sure we initialize the
        // sharding state for this shard.
        if (!shardingState->enabled()) {
            if (cmdObj["configdb"].type() != String) {
                errmsg = "sharding not enabled";
                warning() << errmsg;
                return false;
            }

            const string configdb = cmdObj["configdb"].String();
            shardingState->initializeFromConfigConnString(txn, configdb);
        }

        // Initialize our current shard name in the shard state if needed
        shardingState->setShardName(shardName);

        log() << "received splitChunk request: " << cmdObj;

        //
        // 2. lock the collection's metadata and get highest version for the current shard
        //

        const string whyMessage(str::stream() << "splitting chunk [" << min << ", " << max
                                              << ") in "
                                              << nss.toString());
        auto scopedDistLock = grid.catalogClient(txn)->distLock(
            txn, nss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        if (!scopedDistLock.isOK()) {
            errmsg = str::stream() << "could not acquire collection lock for " << nss.toString()
                                   << " to split chunk [" << min << "," << max << ")"
                                   << causedBy(scopedDistLock.getStatus());
            warning() << errmsg;
            return false;
        }

        // Always check our version remotely
        ChunkVersion shardVersion;
        Status refreshStatus = shardingState->refreshMetadataNow(txn, nss.ns(), &shardVersion);

        if (!refreshStatus.isOK()) {
            errmsg = str::stream() << "splitChunk cannot split chunk "
                                   << "[" << min << "," << max << ")"
                                   << causedBy(refreshStatus.reason());

            warning() << errmsg;
            return false;
        }

        if (shardVersion.majorVersion() == 0) {
            // It makes no sense to split if our version is zero and we have no chunks
            errmsg = str::stream() << "splitChunk cannot split chunk "
                                   << "[" << min << "," << max << ")"
                                   << " with zero shard version";

            warning() << errmsg;
            return false;
        }

        ChunkVersion cmdVersion;
        {
            // Mongos >= v3.2 sends the full version, v3.0 only sends the epoch.
            // TODO(SERVER-20742): Stop parsing epoch separately after 3.2.
            OID cmdEpoch;
            auto& oss = OperationShardingState::get(txn);
            if (oss.hasShardVersion()) {
                cmdVersion = oss.getShardVersion(nss);
                cmdEpoch = cmdVersion.epoch();
            } else {
                BSONElement epochElem(cmdObj["epoch"]);
                if (epochElem.type() == jstOID) {
                    cmdEpoch = epochElem.OID();
                }
            }

            if (cmdEpoch != shardVersion.epoch()) {
                std::string msg = str::stream() << "splitChunk cannot split chunk "
                                                << "[" << min << "," << max << "), "
                                                << "collection may have been dropped. "
                                                << "current epoch: " << shardVersion.epoch()
                                                << ", cmd epoch: " << cmdEpoch;
                warning() << msg;
                throw SendStaleConfigException(nss.toString(), msg, cmdVersion, shardVersion);
            }
        }

        ScopedCollectionMetadata collMetadata;
        {
            AutoGetCollection autoColl(txn, nss, MODE_IS);

            // Get collection metadata
            collMetadata = CollectionShardingState::get(txn, nss.ns())->getMetadata();
        }

        // With nonzero shard version, we must have metadata
        invariant(collMetadata);

        ChunkVersion collVersion = collMetadata->getCollVersion();
        // With nonzero shard version, we must have a coll version >= our shard version
        invariant(collVersion >= shardVersion);

        ChunkType origChunk;
        if (!collMetadata->getNextChunk(min, &origChunk) || origChunk.getMin().woCompare(min) ||
            origChunk.getMax().woCompare(max)) {
            // Our boundaries are different from those passed in
            std::string msg = str::stream() << "splitChunk cannot find chunk "
                                            << "[" << min << "," << max << ")"
                                            << " to split, the chunk boundaries may be stale";
            warning() << msg;
            throw SendStaleConfigException(nss.toString(), msg, cmdVersion, shardVersion);
        }

        log() << "splitChunk accepted at version " << shardVersion;

        //
        // 3. create the batch of updates to metadata ( the new chunks ) to be applied via
        //    'applyOps' command
        //

        BSONObjBuilder logDetail;
        appendShortVersion(logDetail.subobjStart("before"), origChunk);
        LOG(1) << "before split on " << origChunk;
        OwnedPointerVector<ChunkType> newChunks;

        ChunkVersion nextChunkVersion = collVersion;
        BSONObj startKey = min;
        splitKeys.push_back(max);  // makes it easier to have 'max' in the next loop. remove later.

        BSONArrayBuilder updates;

        for (vector<BSONObj>::const_iterator it = splitKeys.begin(); it != splitKeys.end(); ++it) {
            BSONObj endKey = *it;

            if (endKey.woCompare(startKey) == 0) {
                errmsg = str::stream() << "split on the lower bound of chunk "
                                       << "[" << min << ", " << max << ")"
                                       << " is not allowed";

                warning() << errmsg;
                return false;
            }

            // Make sure splits don't create too-big shard keys
            Status status = ShardKeyPattern::checkShardKeySize(endKey);
            if (!status.isOK()) {
                errmsg = status.reason();
                warning() << errmsg;
                return false;
            }

            // splits only update the 'minor' portion of version
            nextChunkVersion.incMinor();

            // build an update operation against the chunks collection of the config database with
            // upsert true
            BSONObjBuilder op;
            op.append("op", "u");
            op.appendBool("b", true);
            op.append("ns", ChunkType::ConfigNS);

            // add the modified (new) chunk information as the update object
            BSONObjBuilder n(op.subobjStart("o"));
            n.append(ChunkType::name(), ChunkType::genID(nss.ns(), startKey));
            nextChunkVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
            n.append(ChunkType::ns(), nss.ns());
            n.append(ChunkType::min(), startKey);
            n.append(ChunkType::max(), endKey);
            n.append(ChunkType::shard(), shardName);
            n.done();

            // add the chunk's _id as the query part of the update statement
            BSONObjBuilder q(op.subobjStart("o2"));
            q.append(ChunkType::name(), ChunkType::genID(nss.ns(), startKey));
            q.done();

            updates.append(op.obj());

            // remember this chunk info for logging later
            unique_ptr<ChunkType> chunk(new ChunkType());
            chunk->setMin(startKey);
            chunk->setMax(endKey);
            chunk->setVersion(nextChunkVersion);

            newChunks.push_back(chunk.release());

            startKey = endKey;
        }

        splitKeys.pop_back();  // 'max' was used as sentinel

        BSONArrayBuilder preCond;
        {
            BSONObjBuilder b;
            b.append("ns", ChunkType::ConfigNS);
            b.append("q",
                     BSON("query" << BSON(ChunkType::ns(nss.ns())) << "orderby"
                                  << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
            {
                BSONObjBuilder bb(b.subobjStart("res"));
                // TODO: For backwards compatibility, we can't yet require an epoch here
                bb.appendTimestamp(ChunkType::DEPRECATED_lastmod(), collVersion.toLong());
                bb.done();
            }
            preCond.append(b.obj());
        }

        //
        // 4. apply the batch of updates to remote and local metadata
        //

        Status applyOpsStatus = grid.catalogClient(txn)->applyChunkOpsDeprecated(
            txn, updates.arr(), preCond.arr(), nss.ns(), nextChunkVersion);
        if (!applyOpsStatus.isOK()) {
            return appendCommandStatus(result, applyOpsStatus);
        }

        //
        // Install chunk metadata with knowledge about newly split chunks in this shard's state
        //

        {
            ScopedTransaction scopedXact(txn, MODE_IX);
            AutoGetCollection autoColl(txn, nss, MODE_IX, MODE_X);

            auto css = CollectionShardingState::get(txn, nss);

            // NOTE: The newShardVersion resulting from this split is higher than any other chunk
            // version, so it's also implicitly the newCollVersion
            ChunkVersion newShardVersion = collVersion;

            // Increment the minor version once, splitChunk increments once per split point
            // (resulting in the correct final shard/collection version)
            //
            // TODO: Revisit this interface, it's a bit clunky
            newShardVersion.incMinor();

            std::unique_ptr<CollectionMetadata> cloned(uassertStatusOK(
                css->getMetadata()->cloneSplit(min, max, splitKeys, newShardVersion)));
            css->setMetadata(std::move(cloned));
        }

        //
        // 5. logChanges
        //

        // single splits are logged different than multisplits
        if (newChunks.size() == 2) {
            appendShortVersion(logDetail.subobjStart("left"), *newChunks[0]);
            appendShortVersion(logDetail.subobjStart("right"), *newChunks[1]);

            grid.catalogClient(txn)->logChange(txn, "split", nss.ns(), logDetail.obj());
        } else {
            BSONObj beforeDetailObj = logDetail.obj();
            BSONObj firstDetailObj = beforeDetailObj.getOwned();
            const int newChunksSize = newChunks.size();

            for (int i = 0; i < newChunksSize; i++) {
                BSONObjBuilder chunkDetail;
                chunkDetail.appendElements(beforeDetailObj);
                chunkDetail.append("number", i + 1);
                chunkDetail.append("of", newChunksSize);
                appendShortVersion(chunkDetail.subobjStart("chunk"), *newChunks[i]);

                grid.catalogClient(txn)->logChange(txn, "multi-split", nss.ns(), chunkDetail.obj());
            }
        }

        dassert(newChunks.size() > 1);

        {
            // Select chunk to move out for "top chunk optimization".
            KeyPattern shardKeyPattern(collMetadata->getKeyPattern());

            AutoGetCollection autoColl(txn, nss, MODE_IS);

            Collection* const collection = autoColl.getCollection();
            if (!collection) {
                warning() << "will not perform top-chunk checking since " << nss.toString()
                          << " does not exist after splitting";
                return true;
            }

            // Allow multiKey based on the invariant that shard keys must be
            // single-valued. Therefore, any multi-key index prefixed by shard
            // key cannot be multikey over the shard key fields.
            IndexDescriptor* idx =
                collection->getIndexCatalog()->findShardKeyPrefixedIndex(txn, keyPattern, false);

            if (idx == NULL) {
                return true;
            }

            const ChunkType* backChunk = newChunks.vector().back();
            const ChunkType* frontChunk = newChunks.vector().front();

            if (shardKeyPattern.globalMax().woCompare(backChunk->getMax()) == 0 &&
                checkIfSingleDoc(txn, collection, idx, backChunk)) {
                result.append("shouldMigrate",
                              BSON("min" << backChunk->getMin() << "max" << backChunk->getMax()));
            } else if (shardKeyPattern.globalMin().woCompare(frontChunk->getMin()) == 0 &&
                       checkIfSingleDoc(txn, collection, idx, frontChunk)) {
                result.append("shouldMigrate",
                              BSON("min" << frontChunk->getMin() << "max" << frontChunk->getMax()));
            }
        }

        return true;
    }

} cmdSplitChunk;

}  // namespace
}  // namespace mongo
