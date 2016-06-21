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

#include "mongo/client/connpool.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/mr.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::map;
using std::set;
using std::string;
using std::vector;

namespace {

AtomicUInt32 JOB_NUMBER;

const Milliseconds kNoDistLockTimeout(-1);

/**
 * Generates a unique name for the temporary M/R output collection.
 */
string getTmpName(StringData coll) {
    return str::stream() << "tmp.mrs." << coll << "_" << time(0) << "_"
                         << JOB_NUMBER.fetchAndAdd(1);
}

/**
 * Given an input map/reduce command, this call generates the matching command which should
 * be sent to the shards as part of the first phase of map/reduce.
 */
BSONObj fixForShards(const BSONObj& orig,
                     const string& output,
                     string& badShardedField,
                     int maxChunkSizeBytes) {
    BSONObjBuilder b;
    BSONObjIterator i(orig);
    while (i.more()) {
        BSONElement e = i.next();
        const string fn = e.fieldName();

        if (fn == bypassDocumentValidationCommandOption() || fn == "map" || fn == "mapreduce" ||
            fn == "mapReduce" || fn == "mapparams" || fn == "reduce" || fn == "query" ||
            fn == "sort" || fn == "collation" || fn == "scope" || fn == "verbose" ||
            fn == "$queryOptions" || fn == "readConcern" ||
            fn == QueryRequest::cmdOptionMaxTimeMS) {
            b.append(e);
        } else if (fn == "out" || fn == "finalize" || fn == "writeConcern") {
            // We don't want to copy these
        } else {
            badShardedField = fn;
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
class MRCmd : public Command {
public:
    MRCmd() : Command("mapReduce", false, "mapreduce") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return mr::mrSupportsWriteConcern(cmd);
    }

    virtual void help(std::stringstream& help) const {
        help << "Runs the sharded map/reduce command";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        mr::addPrivilegesRequiredForMapReduce(this, dbname, cmdObj, out);
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        Timer t;

        const NamespaceString nss(parseNs(dbname, cmdObj));
        uassert(ErrorCodes::InvalidNamespace, "Invalid namespace", nss.isValid());

        const string shardResultCollection = getTmpName(nss.coll());

        bool shardedOutput = false;
        NamespaceString outputCollNss;
        bool customOutDB = false;
        bool inlineOutput = false;

        string outDB = dbname;

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
                const string finalColShort = customOut.firstElement().str();
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

        // Ensure the input database exists
        auto status = grid.catalogCache()->getDatabase(txn, dbname);
        if (!status.isOK()) {
            return appendCommandStatus(result, status.getStatus());
        }

        shared_ptr<DBConfig> confIn = status.getValue();

        shared_ptr<DBConfig> confOut;
        if (customOutDB) {
            // Create the output database implicitly, since we have a custom output requested
            auto scopedDb = uassertStatusOK(ScopedShardDatabase::getOrCreate(txn, outDB));
            confOut = scopedDb.getSharedDbReference();
        } else {
            confOut = confIn;
        }

        if (confOut->getPrimaryId() == "config" && !inlineOutput) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::CommandNotSupported,
                       str::stream() << "Can not execute mapReduce with output database " << outDB
                                     << " which lives on config servers"));
        }

        const bool shardedInput =
            confIn && confIn->isShardingEnabled() && confIn->isSharded(nss.ns());

        if (!shardedOutput) {
            uassert(15920,
                    "Cannot output to a non-sharded collection because "
                    "sharded collection exists already",
                    !confOut->isSharded(outputCollNss.ns()));

            // TODO: Should we also prevent going from non-sharded to sharded? During the
            //       transition client may see partial data.
        }

        int64_t maxChunkSizeBytes = 0;
        if (shardedOutput) {
            // Will need to figure out chunks, ask shards for points
            maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
            if (maxChunkSizeBytes == 0) {
                maxChunkSizeBytes =
                    Grid::get(txn)->getBalancerConfiguration()->getMaxChunkSizeBytes();
            }

            // maxChunkSizeBytes is sent as int BSON field
            invariant(maxChunkSizeBytes < std::numeric_limits<int>::max());
        }

        // modify command to run on shards with output to tmp collection
        string badShardedField;
        BSONObj shardedCommand =
            fixForShards(cmdObj, shardResultCollection, badShardedField, maxChunkSizeBytes);

        if (!shardedInput && !shardedOutput && !customOutDB) {
            LOG(1) << "simple MR, just passthrough";

            const auto shard = grid.shardRegistry()->getShard(txn, confIn->getPrimaryId());
            ShardConnection conn(shard->getConnString(), "");

            BSONObj res;
            bool ok = conn->runCommand(dbname, cmdObj, res);
            conn.done();

            if (auto wcErrorElem = res["writeConcernError"]) {
                appendWriteConcernErrorToCmdResponse(shard->getId(), wcErrorElem, result);
            }

            result.appendElementsUnique(res);
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

        set<string> servers;
        vector<Strategy::CommandResult> mrCommandResults;

        BSONObjBuilder shardResultsB;
        BSONObjBuilder shardCountsB;
        map<string, int64_t> countsMap;
        set<BSONObj> splitPts;

        {
            bool ok = true;

            // TODO: take distributed lock to prevent split / migration?

            try {
                Strategy::commandOp(txn, dbname, shardedCommand, 0, nss.ns(), q, &mrCommandResults);
            } catch (DBException& e) {
                e.addContext(str::stream() << "could not run map command on all shards for ns "
                                           << nss.ns()
                                           << " and query "
                                           << q);
                throw;
            }

            for (const auto& mrResult : mrCommandResults) {
                // Need to gather list of all servers even if an error happened
                string server;
                {
                    const auto shard = grid.shardRegistry()->getShard(txn, mrResult.shardTargetId);
                    server = shard->getConnString().toString();
                }
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
                    vector<BSONElement> pts = splitKeys.Array();
                    for (vector<BSONElement>::iterator it = pts.begin(); it != pts.end(); ++it) {
                        splitPts.insert(it->Obj().getOwned());
                    }
                }
            }

            if (!ok) {
                _cleanUp(servers, dbname, shardResultCollection);

                // Add "code" to the top-level response, if the failure of the sharded command
                // can be accounted to a single error.
                int code = getUniqueCodeFromCommandResults(mrCommandResults);
                if (code != 0) {
                    result.append("code", code);
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
        finalCmd.append("writeConcern", txn->getWriteConcern().toBSON());

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
        bool hasWCError = false;

        if (!shardedOutput) {
            const auto shard = grid.shardRegistry()->getShard(txn, confOut->getPrimaryId());
            LOG(1) << "MR with single shard output, NS=" << outputCollNss.ns()
                   << " primary=" << shard->toString();

            ShardConnection conn(shard->getConnString(), outputCollNss.ns());
            ok = conn->runCommand(outDB, finalCmd.obj(), singleResult);

            BSONObj counts = singleResult.getObjectField("counts");
            postCountsB.append(conn->getServerAddress(), counts);
            reduceCount = counts.getIntField("reduce");
            outputCount = counts.getIntField("output");

            conn.done();
            if (!hasWCError) {
                if (auto wcErrorElem = singleResult["writeConcernError"]) {
                    appendWriteConcernErrorToCmdResponse(shard->getId(), wcErrorElem, result);
                    hasWCError = true;
                }
            }
        } else {
            LOG(1) << "MR with sharded output, NS=" << outputCollNss.ns();

            // Create the sharded collection if needed
            if (!confOut->isSharded(outputCollNss.ns())) {
                // Enable sharding on db
                confOut->enableSharding(txn);

                // Shard collection according to split points
                vector<BSONObj> sortedSplitPts;

                // Points will be properly sorted using the set
                for (const auto& splitPt : splitPts) {
                    sortedSplitPts.push_back(splitPt);
                }

                // Pre-split the collection onto all the shards for this database. Note that
                // it's not completely safe to pre-split onto non-primary shards using the
                // shardcollection method (a conflict may result if multiple map-reduces are
                // writing to the same output collection, for instance).
                //
                // TODO: pre-split mapReduce output in a safer way.

                set<ShardId> outShardIds;
                confOut->getAllShardIds(&outShardIds);

                BSONObj sortKey = BSON("_id" << 1);
                ShardKeyPattern sortKeyPattern(sortKey);
                Status status = grid.catalogClient(txn)->shardCollection(
                    txn, outputCollNss.ns(), sortKeyPattern, true, sortedSplitPts, outShardIds);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }

                // Make sure the cached metadata for the collection knows that we are now sharded
                confOut = uassertStatusOK(
                    grid.catalogCache()->getDatabase(txn, outputCollNss.db().toString()));
                confOut->getChunkManager(txn, outputCollNss.ns(), true /* force */);
            }

            map<BSONObj, int> chunkSizes;
            {
                // Take distributed lock to prevent split / migration.
                auto scopedDistLock = grid.catalogClient(txn)->distLock(
                    txn, outputCollNss.ns(), "mr-post-process", kNoDistLockTimeout);

                if (!scopedDistLock.isOK()) {
                    return appendCommandStatus(result, scopedDistLock.getStatus());
                }

                BSONObj finalCmdObj = finalCmd.obj();
                mrCommandResults.clear();

                try {
                    Strategy::commandOp(txn,
                                        outDB,
                                        finalCmdObj,
                                        0,
                                        outputCollNss.ns(),
                                        BSONObj(),
                                        &mrCommandResults);
                    ok = true;
                } catch (DBException& e) {
                    e.addContext(str::stream() << "could not run final reduce on all shards for "
                                               << nss.ns()
                                               << ", output "
                                               << outputCollNss.ns());
                    throw;
                }

                for (const auto& mrResult : mrCommandResults) {
                    string server;
                    {
                        const auto shard =
                            grid.shardRegistry()->getShard(txn, mrResult.shardTargetId);
                        server = shard->getConnString().toString();
                    }
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
                        vector<BSONElement> sizes = singleResult.getField("chunkSizes").Array();
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
            shared_ptr<ChunkManager> cm = confOut->getChunkManagerIfExists(txn, outputCollNss.ns());
            uassert(34359,
                    str::stream() << "Failed to write mapreduce output to " << outputCollNss.ns()
                                  << "; expected that collection to be sharded, but it was not",
                    cm);

            for (const auto& chunkSize : chunkSizes) {
                BSONObj key = chunkSize.first;
                const int size = chunkSize.second;
                invariant(size < std::numeric_limits<int>::max());

                // key reported should be the chunk's minimum
                shared_ptr<Chunk> c = cm->findIntersectingChunk(txn, key);
                if (!c) {
                    warning() << "Mongod reported " << size << " bytes inserted for key " << key
                              << " but can't find chunk";
                } else {
                    c->splitIfShould(txn, size);
                }
            }
        }

        _cleanUp(servers, dbname, shardResultCollection);

        if (!ok) {
            errmsg = str::stream() << "MR post processing failed: " << singleResult.toString();
            return 0;
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
     * Drops the temporary results collections from each shard.
     */
    void _cleanUp(const set<string>& servers, string dbName, string shardResultCollection) {
        try {
            // drop collections with tmp results on each shard
            for (const auto& server : servers) {
                ScopedDbConnection conn(server);
                conn->dropCollection(dbName + "." + shardResultCollection);
                conn.done();
            }
        } catch (const DBException& e) {
            warning() << "Cannot cleanup shard results" << e.toString();
        } catch (const std::exception& e) {
            severe() << "Cannot cleanup shard results" << causedBy(e);
        }
    }

} clusterMapReduceCmd;

}  // namespace
}  // namespace mongo
