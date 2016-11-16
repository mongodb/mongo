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

#include "mongo/bson/util/bson_extract.h"
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
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/split_chunk_request_type.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

namespace {

const char kChunkVersion[] = "chunkVersion";

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

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
                                                             BoundInclusion::kIncludeStartKeyOnly,
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

//
// Checks the collection's metadata for a successful split on the specified chunkRange
// using the specified splitPoints. Returns false if the metadata's chunks don't match
// the new chunk boundaries exactly.
//
bool _checkMetadataForSuccess(OperationContext* txn,
                              const NamespaceString& nss,
                              const ChunkRange& chunkRange,
                              const std::vector<BSONObj>& splitKeys) {
    ScopedCollectionMetadata metadataAfterSplit;
    {
        AutoGetCollection autoColl(txn, nss, MODE_IS);

        // Get collection metadata
        metadataAfterSplit = CollectionShardingState::get(txn, nss.ns())->getMetadata();
    }

    auto newChunkBounds(splitKeys);
    auto startKey = chunkRange.getMin();
    newChunkBounds.push_back(chunkRange.getMax());

    ChunkType nextChunk;
    for (const auto& endKey : newChunkBounds) {
        // Check that all new chunks fit the new chunk boundaries
        if (!metadataAfterSplit->getNextChunk(startKey, &nextChunk) ||
            nextChunk.getMax().woCompare(endKey)) {
            return false;
        }

        startKey = endKey;
    }

    return true;
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

    Status checkAuthForCommand(Client* client,
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
        // Check whether parameters passed to splitChunk are sound
        //
        const NamespaceString nss = NamespaceString(parseNs(dbname, cmdObj));
        if (!nss.isValid()) {
            errmsg = str::stream() << "invalid namespace '" << nss.toString()
                                   << "' specified for command";
            return false;
        }

        BSONObj keyPatternObj;
        {
            BSONElement keyPatternElem;
            auto keyPatternStatus =
                bsonExtractTypedField(cmdObj, "keyPattern", Object, &keyPatternElem);

            if (!keyPatternStatus.isOK()) {
                errmsg = "need to specify the key pattern the collection is sharded over";
                return false;
            }
            keyPatternObj = keyPatternElem.Obj();
        }

        auto chunkRange = uassertStatusOK(ChunkRange::fromBSON(cmdObj));
        const BSONObj min = chunkRange.getMin();
        const BSONObj max = chunkRange.getMax();

        boost::optional<ChunkVersion> expectedChunkVersion;
        auto statusWithChunkVersion =
            ChunkVersion::parseFromBSONWithFieldForCommands(cmdObj, kChunkVersion);
        if (statusWithChunkVersion.isOK()) {
            expectedChunkVersion = std::move(statusWithChunkVersion.getValue());
        } else if (statusWithChunkVersion != ErrorCodes::NoSuchKey) {
            uassertStatusOK(statusWithChunkVersion);
        }

        vector<BSONObj> splitKeys;
        {
            BSONElement splitKeysElem;
            auto splitKeysElemStatus =
                bsonExtractTypedField(cmdObj, "splitKeys", mongo::Array, &splitKeysElem);

            if (!splitKeysElemStatus.isOK()) {
                errmsg = "need to provide the split points to chunk over";
                return false;
            }
            BSONObjIterator it(splitKeysElem.Obj());
            while (it.more()) {
                splitKeys.push_back(it.next().Obj().getOwned());
            }
        }

        string shardName;
        auto parseShardNameStatus = bsonExtractStringField(cmdObj, "from", &shardName);
        if (!parseShardNameStatus.isOK())
            return appendCommandStatus(result, parseShardNameStatus);

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
            shardingState->initializeFromConfigConnString(txn, configdb, shardName);
        }

        log() << "received splitChunk request: " << redact(cmdObj);

        //
        // Lock the collection's metadata and get highest version for the current shard
        // TODO(SERVER-25086): Remove distLock acquisition from split chunk
        //
        const string whyMessage(str::stream() << "splitting chunk [" << min << ", " << max
                                              << ") in "
                                              << nss.toString());
        auto scopedDistLock = grid.catalogClient(txn)->getDistLockManager()->lock(
            txn, nss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        if (!scopedDistLock.isOK()) {
            errmsg = str::stream() << "could not acquire collection lock for " << nss.toString()
                                   << " to split chunk [" << redact(min) << "," << redact(max)
                                   << ") " << causedBy(redact(scopedDistLock.getStatus()));
            warning() << errmsg;
            return false;
        }

        // Always check our version remotely
        ChunkVersion shardVersion;
        Status refreshStatus = shardingState->refreshMetadataNow(txn, nss, &shardVersion);

        if (!refreshStatus.isOK()) {
            errmsg = str::stream() << "splitChunk cannot split chunk "
                                   << "[" << redact(min) << "," << redact(max) << ") "
                                   << causedBy(redact(refreshStatus));

            warning() << errmsg;
            return false;
        }

        if (shardVersion.majorVersion() == 0) {
            // It makes no sense to split if our version is zero and we have no chunks
            errmsg = str::stream() << "splitChunk cannot split chunk "
                                   << "[" << redact(min) << "," << redact(max) << ") "
                                   << " with zero shard version";

            warning() << errmsg;
            return false;
        }

        const auto& oss = OperationShardingState::get(txn);
        uassert(ErrorCodes::InvalidOptions, "collection version is missing", oss.hasShardVersion());

        // Even though the splitChunk command transmits a value in the operation's shardVersion
        // field, this value does not actually contain the shard version, but the global collection
        // version.
        ChunkVersion expectedCollectionVersion = oss.getShardVersion(nss);
        if (expectedCollectionVersion.epoch() != shardVersion.epoch()) {
            std::string msg = str::stream() << "splitChunk cannot split chunk "
                                            << "[" << redact(min) << "," << redact(max) << "), "
                                            << "collection may have been dropped. "
                                            << "current epoch: " << shardVersion.epoch()
                                            << ", cmd epoch: " << expectedCollectionVersion.epoch();
            warning() << msg;
            throw SendStaleConfigException(
                nss.toString(), msg, expectedCollectionVersion, shardVersion);
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

        {
            ChunkType chunkToMove;
            chunkToMove.setMin(min);
            chunkToMove.setMax(max);
            if (expectedChunkVersion) {
                chunkToMove.setVersion(*expectedChunkVersion);
            }

            uassertStatusOK(collMetadata->checkChunkIsValid(chunkToMove));
        }

        auto request = SplitChunkRequest(
            nss, shardName, expectedCollectionVersion.epoch(), chunkRange, splitKeys);

        auto configCmdObj =
            request.toConfigCommandBSON(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

        auto cmdResponseStatus =
            Grid::get(txn)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
                txn,
                kPrimaryOnlyReadPreference,
                "admin",
                configCmdObj,
                Shard::RetryPolicy::kIdempotent);

        //
        // Refresh chunk metadata regardless of whether or not the split succeeded
        //
        {
            ChunkVersion unusedShardVersion;
            refreshStatus = shardingState->refreshMetadataNow(txn, nss, &unusedShardVersion);

            if (!refreshStatus.isOK()) {
                errmsg = str::stream() << "failed to refresh metadata for split chunk ["
                                       << redact(min) << "," << redact(max) << ") "
                                       << causedBy(redact(refreshStatus));

                warning() << errmsg;
                return false;
            }
        }

        // If we failed to get any response from the config server at all, despite retries, then we
        // should just go ahead and fail the whole operation.
        if (!cmdResponseStatus.isOK())
            return appendCommandStatus(result, cmdResponseStatus.getStatus());

        // Check commandStatus and writeConcernStatus
        auto commandStatus = cmdResponseStatus.getValue().commandStatus;
        auto writeConcernStatus = cmdResponseStatus.getValue().writeConcernStatus;

        // Send stale epoch if epoch of request did not match epoch of collection
        if (commandStatus == ErrorCodes::StaleEpoch) {
            std::string msg = str::stream() << "splitChunk cannot split chunk "
                                            << "[" << redact(min) << "," << redact(max) << "), "
                                            << "collection may have been dropped. "
                                            << "current epoch: " << collVersion.epoch()
                                            << ", cmd epoch: " << expectedCollectionVersion.epoch();
            warning() << msg;

            throw SendStaleConfigException(
                nss.toString(), msg, expectedCollectionVersion, collVersion);

            return appendCommandStatus(result, commandStatus);
        }

        //
        // If _configsvrCommitChunkSplit returned an error, look at this shard's metadata to
        // determine if  the split actually did happen. This can happen if there's a network error
        // getting the response from the first call to _configsvrCommitChunkSplit, but it actually
        // succeeds, thus the automatic retry fails with a precondition violation, for example.
        //
        if ((!commandStatus.isOK() || !writeConcernStatus.isOK()) &&
            _checkMetadataForSuccess(txn, nss, chunkRange, splitKeys)) {

            LOG(1) << "splitChunk [" << redact(min) << "," << redact(max)
                   << ") has already been committed.";
        } else if (!commandStatus.isOK()) {
            return appendCommandStatus(result, commandStatus);
        } else if (!writeConcernStatus.isOK()) {
            return appendCommandStatus(result, writeConcernStatus);
        }

        // Select chunk to move out for "top chunk optimization".
        KeyPattern shardKeyPattern(collMetadata->getKeyPattern());

        AutoGetCollection autoColl(txn, nss, MODE_IS);

        Collection* const collection = autoColl.getCollection();
        if (!collection) {
            warning() << "will not perform top-chunk checking since " << nss.toString()
                      << " does not exist after splitting";
            return true;
        }

        // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore,
        // any multi-key index prefixed by shard key cannot be multikey over the shard key fields.
        IndexDescriptor* idx =
            collection->getIndexCatalog()->findShardKeyPrefixedIndex(txn, keyPatternObj, false);
        if (!idx) {
            return true;
        }

        auto backChunk = ChunkType();
        backChunk.setMin(splitKeys.back());
        backChunk.setMax(max);

        auto frontChunk = ChunkType();
        frontChunk.setMin(min);
        frontChunk.setMax(splitKeys.front());

        if (shardKeyPattern.globalMax().woCompare(backChunk.getMax()) == 0 &&
            checkIfSingleDoc(txn, collection, idx, &backChunk)) {
            result.append("shouldMigrate",
                          BSON("min" << backChunk.getMin() << "max" << backChunk.getMax()));
        } else if (shardKeyPattern.globalMin().woCompare(frontChunk.getMin()) == 0 &&
                   checkIfSingleDoc(txn, collection, idx, &frontChunk)) {
            result.append("shouldMigrate",
                          BSON("min" << frontChunk.getMin() << "max" << frontChunk.getMax()));
        }

        return true;
    }

} cmdSplitChunk;

}  // namespace
}  // namespace mongo
