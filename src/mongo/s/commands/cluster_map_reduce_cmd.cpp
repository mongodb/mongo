/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/connpool.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/mr.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_write.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

AtomicUInt32 JOB_NUMBER;

const Milliseconds kNoDistLockTimeout(-1);

/**
 * Generates a unique name for the temporary M/R output collection.
 */
std::string getTmpName(StringData coll) {
    return str::stream() << "tmp.mrs." << coll << "_" << time(0) << "_"
                         << JOB_NUMBER.fetchAndAdd(1);
}

/**
 * Given an input map/reduce command, this call generates the matching command which should
 * be sent to the shards as part of the first phase of map/reduce.
 */
BSONObj fixForShards(const BSONObj& orig,
                     const std::string& output,
                     std::string& badShardedField,
                     int maxChunkSizeBytes) {
    BSONObjBuilder b;
    BSONObjIterator i(orig);
    while (i.more()) {
        BSONElement e = i.next();
        const auto fn = e.fieldNameStringData();

        if (fn == bypassDocumentValidationCommandOption() || fn == "map" || fn == "mapreduce" ||
            fn == "mapReduce" || fn == "mapparams" || fn == "reduce" || fn == "query" ||
            fn == "sort" || fn == "collation" || fn == "scope" || fn == "verbose" ||
            fn == "$queryOptions" || fn == "readConcern" ||
            fn == QueryRequest::cmdOptionMaxTimeMS) {
            b.append(e);
        } else if (fn == "out" || fn == "finalize" || fn == "writeConcern") {
            // We don't want to copy these
        } else if (!Command::isGenericArgument(fn)) {
            badShardedField = fn.toString();
            return BSONObj();
        }
    }

    b.append("out", output);
    b.append("shardedFirstPass", true);

    if (maxChunkSizeBytes > 0) {
        // Will need to figure out chunks, ask shards for points
        b.append("splitInfo", maxChunkSizeBytes);
    }

    return b.obj();
}

/**
 * Outline for sharded map reduce for sharded output, $out replace:
 *
 * ============= mongos =============
 * 1. Send map reduce command to all relevant shards with some extra info like the value for
 *    the chunkSize and the name of the temporary output collection.
 *
 * ============= shard =============
 * 2. Does normal map reduce.
 *
 * 3. Calls splitVector on itself against the output collection and puts the results into the
 *    response object.
 *
 * ============= mongos =============
 * 4. If the output collection is *not* sharded, uses the information from splitVector to
 *    create a pre-split sharded collection.
 *
 * 5. Grabs the distributed lock for the final output collection.
 *
 * 6. Sends mapReduce.shardedfinish.
 *
 * ============= shard =============
 * 7. Extracts the list of shards from the mapReduce.shardedfinish and performs a broadcast
 *    query against all of them to obtain all documents that this shard owns.
 *
 * 8. Performs the reduce operation against every document from step #7 and outputs them to
 *    another temporary collection. Also keeps track of the BSONObject size of every "reduced"
 *    document for each chunk range.
 *
 * 9. Atomically drops the old output collection and renames the temporary collection to the
 *    output collection.
 *
 * ============= mongos =============
 * 10. Releases the distributed lock acquired at step #5.
 *
 * 11. Inspects the BSONObject size from step #8 and determines if it needs to split.
 */
class MRCmd : public ErrmsgCommandDeprecated {
public:
    MRCmd() : ErrmsgCommandDeprecated("mapReduce", "mapreduce") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return mr::mrSupportsWriteConcern(cmd);
    }

    void help(std::stringstream& help) const override {
        help << "Runs the sharded map/reduce command";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        mr::addPrivilegesRequiredForMapReduce(this, dbname, cmdObj, out);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        Timer t;

        const NamespaceString nss(parseNs(dbname, cmdObj));
        const std::string shardResultCollection = getTmpName(nss.coll());

        bool shardedOutput = false;
        bool customOutDB = false;
        NamespaceString outputCollNss;
        bool inlineOutput = false;

        std::string outDB = dbname;

        BSONElement outElmt = cmdObj.getField("out");
        if (outElmt.type() == Object) {
            // Check if there is a custom output
            BSONObj customOut = outElmt.embeddedObject();
            shardedOutput = customOut.getBoolField("sharded");

            if (customOut.hasField("inline")) {
                inlineOutput = true;
                uassert(ErrorCodes::InvalidOptions,
                        "cannot specify inline and sharded output at the same time",
                        !shardedOutput);
                uassert(ErrorCodes::InvalidOptions,
                        "cannot specify inline and output database at the same time",
                        !customOut.hasField("db"));
            } else {
                // Mode must be 1st element
                const std::string finalColShort = customOut.firstElement().str();

                if (customOut.hasField("db")) {
                    customOutDB = true;
                    outDB = customOut.getField("db").str();
                }

                outputCollNss = NamespaceString(outDB, finalColShort);
                uassert(ErrorCodes::InvalidNamespace,
                        "Invalid output namespace",
                        outputCollNss.isValid());
            }
        }

        auto const catalogCache = Grid::get(opCtx)->catalogCache();

        // Ensure the input database exists and set up the input collection
        auto inputRoutingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));

        const bool shardedInput = inputRoutingInfo.cm() != nullptr;

        // Create the output database implicitly if we have a custom output requested
        if (customOutDB) {
            uassertStatusOK(createShardDatabase(opCtx, outDB));
        }

        // Ensure that the output database doesn't reside on the config server
        auto outputDbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, outDB));
        uassert(ErrorCodes::CommandNotSupported,
                str::stream() << "Can not execute mapReduce with output database " << outDB
                              << " which lives on config servers",
                inlineOutput || outputDbInfo.primaryId() != "config");

        int64_t maxChunkSizeBytes = 0;

        if (shardedOutput) {
            // Will need to figure out chunks, ask shards for points
            maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
            if (maxChunkSizeBytes == 0) {
                maxChunkSizeBytes =
                    Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes();
            }

            // maxChunkSizeBytes is sent as int BSON field
            invariant(maxChunkSizeBytes < std::numeric_limits<int>::max());
        } else if (outputCollNss.isValid()) {
            auto outputRoutingInfo =
                uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, outputCollNss));

            uassert(15920,
                    "Cannot output to a non-sharded collection because "
                    "sharded collection exists already",
                    !outputRoutingInfo.cm());

            // TODO: Should we also prevent going from non-sharded to sharded? During the
            //       transition client may see partial data.
        }

        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

        // modify command to run on shards with output to tmp collection
        std::string badShardedField;
        BSONObj shardedCommand =
            fixForShards(cmdObj, shardResultCollection, badShardedField, maxChunkSizeBytes);

        if (!shardedInput && !shardedOutput && !customOutDB) {
            LOG(1) << "simple MR, just passthrough";

            invariant(inputRoutingInfo.primary());

            ShardConnection conn(inputRoutingInfo.primary()->getConnString(), "");

            BSONObj res;
            bool ok = conn->runCommand(dbname, filterCommandRequestForPassthrough(cmdObj), res);
            conn.done();

            if (auto wcErrorElem = res["writeConcernError"]) {
                appendWriteConcernErrorToCmdResponse(
                    inputRoutingInfo.primary()->getId(), wcErrorElem, result);
            }

            result.appendElementsUnique(filterCommandReplyForPassthrough(res));
            return ok;
        }

        if (badShardedField.size()) {
            errmsg = str::stream() << "unknown m/r field for sharding: " << badShardedField;
            return false;
        }

        BSONObj q;
        if (cmdObj["query"].type() == Object) {
            q = cmdObj["query"].embeddedObjectUserCheck();
        }

        BSONObj collation;
        if (cmdObj["collation"].type() == Object) {
            collation = cmdObj["collation"].embeddedObjectUserCheck();
        }

        std::set<std::string> servers;
        std::vector<Strategy::CommandResult> mrCommandResults;

        BSONObjBuilder shardResultsB;
        BSONObjBuilder shardCountsB;
        std::map<std::string, int64_t> countsMap;

        auto splitPts = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

        {
            bool ok = true;

            // TODO: take distributed lock to prevent split / migration?

            try {
                Strategy::commandOp(
                    opCtx, dbname, shardedCommand, nss.ns(), q, collation, &mrCommandResults);
            } catch (DBException& e) {
                e.addContext(str::stream() << "could not run map command on all shards for ns "
                                           << nss.ns()
                                           << " and query "
                                           << q);
                throw;
            }

            for (const auto& mrResult : mrCommandResults) {
                // Need to gather list of all servers even if an error happened
                const auto server = [&]() {
                    const auto shard =
                        uassertStatusOK(shardRegistry->getShard(opCtx, mrResult.shardTargetId));
                    return shard->getConnString().toString();
                }();

                servers.insert(server);

                if (!ok) {
                    continue;
                }

                BSONObj singleResult = mrResult.result;
                ok = singleResult["ok"].trueValue();

                if (!ok) {
                    // At this point we will return
                    errmsg = str::stream() << "MR parallel processing failed: "
                                           << singleResult.toString();
                    continue;
                }

                shardResultsB.append(server, singleResult);

                BSONObj counts = singleResult["counts"].embeddedObjectUserCheck();
                shardCountsB.append(server, counts);

                // Add up the counts for each shard. Some of them will be fixed later like
                // output and reduce.
                BSONObjIterator j(counts);
                while (j.more()) {
                    BSONElement temp = j.next();
                    countsMap[temp.fieldName()] += temp.numberLong();
                }

                if (singleResult.hasField("splitKeys")) {
                    BSONElement splitKeys = singleResult.getField("splitKeys");
                    for (const auto& splitPt : splitKeys.Array()) {
                        splitPts.insert(splitPt.Obj().getOwned());
                    }
                }
            }

            if (!ok) {
                cleanUp(servers, dbname, shardResultCollection);

                // Add "code" to the top-level response, if the failure of the sharded command
                // can be accounted to a single error.
                int code = getUniqueCodeFromCommandResults(mrCommandResults);
                if (code != 0) {
                    result.append("code", code);
                    result.append("codeName", ErrorCodes::errorString(ErrorCodes::fromInt(code)));
                }

                return false;
            }
        }

        // Build the sharded finish command
        BSONObjBuilder finalCmd;
        finalCmd.append("mapreduce.shardedfinish", cmdObj);
        finalCmd.append("inputDB", dbname);
        finalCmd.append("shardedOutputCollection", shardResultCollection);
        finalCmd.append("shards", shardResultsB.done());
        finalCmd.append("writeConcern", opCtx->getWriteConcern().toBSON());

        BSONObj shardCounts = shardCountsB.done();
        finalCmd.append("shardCounts", shardCounts);

        BSONObjBuilder timingBuilder;
        timingBuilder.append("shardProcessing", t.millis());

        BSONObjBuilder aggCountsB;
        for (const auto& countEntry : countsMap) {
            aggCountsB.append(countEntry.first, static_cast<long long>(countEntry.second));
        }

        BSONObj aggCounts = aggCountsB.done();
        finalCmd.append("counts", aggCounts);

        if (auto elem = cmdObj[QueryRequest::cmdOptionMaxTimeMS])
            finalCmd.append(elem);
        if (auto elem = cmdObj[bypassDocumentValidationCommandOption()])
            finalCmd.append(elem);

        Timer t2;

        long long reduceCount = 0;
        long long outputCount = 0;
        BSONObjBuilder postCountsB;

        bool ok = true;
        BSONObj singleResult;

        if (!shardedOutput) {
            LOG(1) << "MR with single shard output, NS=" << outputCollNss
                   << " primary=" << outputDbInfo.primaryId();

            const auto outputShard =
                uassertStatusOK(shardRegistry->getShard(opCtx, outputDbInfo.primaryId()));

            ShardConnection conn(outputShard->getConnString(), outputCollNss.ns());
            ok = conn->runCommand(outDB, finalCmd.obj(), singleResult);

            BSONObj counts = singleResult.getObjectField("counts");
            postCountsB.append(conn->getServerAddress(), counts);
            reduceCount = counts.getIntField("reduce");
            outputCount = counts.getIntField("output");

            conn.done();

            if (auto wcErrorElem = singleResult["writeConcernError"]) {
                appendWriteConcernErrorToCmdResponse(outputShard->getId(), wcErrorElem, result);
            }
        } else {
            LOG(1) << "MR with sharded output, NS=" << outputCollNss.ns();

            auto outputRoutingInfo =
                uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, outputCollNss));

            // Create the sharded collection if needed
            if (!outputRoutingInfo.cm()) {
                outputRoutingInfo = createShardedOutputCollection(opCtx, outputCollNss, splitPts);
            }

            auto chunkSizes = SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<int>();
            {
                // Take distributed lock to prevent split / migration.
                auto scopedDistLock = Grid::get(opCtx)->catalogClient()->getDistLockManager()->lock(
                    opCtx, outputCollNss.ns(), "mr-post-process", kNoDistLockTimeout);
                if (!scopedDistLock.isOK()) {
                    return appendCommandStatus(result, scopedDistLock.getStatus());
                }

                BSONObj finalCmdObj = finalCmd.obj();
                mrCommandResults.clear();

                try {
                    const BSONObj query;
                    Strategy::commandOp(opCtx,
                                        outDB,
                                        finalCmdObj,
                                        outputCollNss.ns(),
                                        query,
                                        CollationSpec::kSimpleSpec,
                                        &mrCommandResults);
                    ok = true;
                } catch (DBException& e) {
                    e.addContext(str::stream() << "could not run final reduce on all shards for "
                                               << nss.ns()
                                               << ", output "
                                               << outputCollNss.ns());
                    throw;
                }

                bool hasWCError = false;

                for (const auto& mrResult : mrCommandResults) {
                    const auto server = [&]() {
                        const auto shard =
                            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(
                                opCtx, mrResult.shardTargetId));
                        return shard->getConnString().toString();
                    }();

                    singleResult = mrResult.result;
                    if (!hasWCError) {
                        if (auto wcErrorElem = singleResult["writeConcernError"]) {
                            appendWriteConcernErrorToCmdResponse(
                                mrResult.shardTargetId, wcErrorElem, result);
                            hasWCError = true;
                        }
                    }

                    ok = singleResult["ok"].trueValue();
                    if (!ok) {
                        break;
                    }

                    BSONObj counts = singleResult.getObjectField("counts");
                    reduceCount += counts.getIntField("reduce");
                    outputCount += counts.getIntField("output");
                    postCountsB.append(server, counts);

                    // get the size inserted for each chunk
                    // split cannot be called here since we already have the distributed lock
                    if (singleResult.hasField("chunkSizes")) {
                        std::vector<BSONElement> sizes =
                            singleResult.getField("chunkSizes").Array();
                        for (unsigned int i = 0; i < sizes.size(); i += 2) {
                            BSONObj key = sizes[i].Obj().getOwned();
                            const long long size = sizes[i + 1].numberLong();

                            invariant(size < std::numeric_limits<int>::max());
                            chunkSizes[key] = static_cast<int>(size);
                        }
                    }
                }
            }

            // Do the splitting round
            catalogCache->onStaleConfigError(std::move(outputRoutingInfo));
            outputRoutingInfo =
                uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, outputCollNss));
            uassert(34359,
                    str::stream() << "Failed to write mapreduce output to " << outputCollNss.ns()
                                  << "; expected that collection to be sharded, but it was not",
                    outputRoutingInfo.cm());

            const auto outputCM = outputRoutingInfo.cm();

            for (const auto& chunkSize : chunkSizes) {
                BSONObj key = chunkSize.first;
                const int size = chunkSize.second;
                invariant(size < std::numeric_limits<int>::max());

                // Key reported should be the chunk's minimum
                auto c = outputCM->findIntersectingChunkWithSimpleCollation(key);
                if (!c) {
                    warning() << "Mongod reported " << size << " bytes inserted for key " << key
                              << " but can't find chunk";
                } else {
                    updateChunkWriteStatsAndSplitIfNeeded(opCtx, outputCM.get(), c.get(), size);
                }
            }
        }

        cleanUp(servers, dbname, shardResultCollection);

        if (!ok) {
            errmsg = str::stream() << "MR post processing failed: " << singleResult.toString();
            return false;
        }

        // copy some elements from a single result
        // annoying that we have to copy all results for inline, but no way around it
        if (singleResult.hasField("result")) {
            result.append(singleResult.getField("result"));
        } else if (singleResult.hasField("results")) {
            result.append(singleResult.getField("results"));
        }

        BSONObjBuilder countsB(32);
        // input stat is determined by aggregate MR job
        countsB.append("input", aggCounts.getField("input").numberLong());
        countsB.append("emit", aggCounts.getField("emit").numberLong());

        // reduce count is sum of all reduces that happened
        countsB.append("reduce", aggCounts.getField("reduce").numberLong() + reduceCount);

        // ouput is determined by post processing on each shard
        countsB.append("output", outputCount);
        result.append("counts", countsB.done());

        timingBuilder.append("postProcessing", t2.millis());

        result.append("timeMillis", t.millis());
        result.append("timing", timingBuilder.done());
        result.append("shardCounts", shardCounts);
        result.append("postProcessCounts", postCountsB.done());

        return true;
    }

private:
    /**
     * Creates and shards the collection for the output results.
     */
    static CachedCollectionRoutingInfo createShardedOutputCollection(OperationContext* opCtx,
                                                                     const NamespaceString& nss,
                                                                     const BSONObjSet& splitPts) {
        auto const catalogCache = Grid::get(opCtx)->catalogCache();

        // Enable sharding on the output db
        auto status =
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                "admin",
                BSON("_configsvrEnableSharding" << nss.db().toString()),
                Shard::RetryPolicy::kIdempotent);

        // If the database has sharding already enabled, we can ignore the error
        if (status.isOK()) {
            // Invalidate the output database so it gets reloaded on the next fetch attempt
            catalogCache->purgeDatabase(nss.db());
        } else if (status != ErrorCodes::AlreadyInitialized) {
            uassertStatusOK(status);
        }

        // Points will be properly sorted using the set
        const std::vector<BSONObj> sortedSplitPts(splitPts.begin(), splitPts.end());

        // Specifying the initial split points explicitly will cause _configsvrShardCollection to
        // distribute the initial chunks evenly across shards.
        // Note that it's not safe to pre-split onto non-primary shards through shardCollection:
        // a conflict may result if multiple map-reduces are writing to the same output collection,
        //
        // TODO: pre-split mapReduce output in a safer way.

        // Invalidate the routing table cache entry for this collection so that we reload the
        // collection the next time it's accessed, even if we receive a failure, e.g. NetworkError.
        ON_BLOCK_EXIT([catalogCache, nss] { catalogCache->invalidateShardedCollection(nss); });

        ConfigsvrShardCollection configShardCollRequest;
        configShardCollRequest.set_configsvrShardCollection(nss);
        configShardCollRequest.setKey(BSON("_id" << 1));
        configShardCollRequest.setUnique(true);
        // TODO (SERVER-29622): Setting the numInitialChunks to 0 will be unnecessary once the
        // constructor automatically respects default values specified in the .idl.
        configShardCollRequest.setNumInitialChunks(0);
        configShardCollRequest.setInitialSplitPoints(sortedSplitPts);

        auto cmdResponse = uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                "admin",
                configShardCollRequest.toBSON(),
                Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponse.commandStatus);

        // Make sure the cached metadata for the collection knows that we are now sharded
        return uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
    }

    /**
     * Drops the temporary results collections from each shard.
     */
    static void cleanUp(const std::set<std::string>& servers,
                        const std::string& dbName,
                        const std::string& shardResultCollection) {
        try {
            // drop collections with tmp results on each shard
            for (const auto& server : servers) {
                ScopedDbConnection conn(server);
                conn->dropCollection(dbName + "." + shardResultCollection);
                conn.done();
            }
        } catch (const DBException& e) {
            warning() << "Cannot cleanup shard results" << redact(e);
        } catch (const std::exception& e) {
            severe() << "Cannot cleanup shard results" << causedBy(redact(e.what()));
        }
    }

} clusterMapReduceCmd;

}  // namespace
}  // namespace mongo
