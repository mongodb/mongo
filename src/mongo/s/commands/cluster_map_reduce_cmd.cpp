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
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/config.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/strategy.h"
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

/**
 * Generates a unique name for the temporary M/R output collection.
 */
string getTmpName(const string& coll) {
    StringBuilder sb;
    sb << "tmp.mrs." << coll << "_" << time(0) << "_" << JOB_NUMBER.fetchAndAdd(1);
    return sb.str();
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
            fn == "sort" || fn == "scope" || fn == "verbose" || fn == "$queryOptions" ||
            fn == LiteParsedQuery::cmdOptionMaxTimeMS) {
            b.append(e);
        } else if (fn == "out" || fn == "finalize") {
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

    virtual bool isWriteCommandForConfigServer() const {
        return false;
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

        const string collection = cmdObj.firstElement().valuestrsafe();
        const string fullns = dbname + "." + collection;
        const string shardResultCollection = getTmpName(collection);

        BSONObj customOut;
        string finalColShort;
        string finalColLong;
        bool customOutDB = false;

        string outDB = dbname;

        BSONElement outElmt = cmdObj.getField("out");
        if (outElmt.type() == Object) {
            // Check if there is a custom output
            BSONObj out = outElmt.embeddedObject();
            customOut = out;

            // Mode must be 1st element
            finalColShort = out.firstElement().str();
            if (customOut.hasField("db")) {
                customOutDB = true;
                outDB = customOut.getField("db").str();
            }

            finalColLong = outDB + "." + finalColShort;
        }

        // Ensure the input database exists
        auto status = grid.catalogCache()->getDatabase(dbname);
        if (!status.isOK()) {
            return appendCommandStatus(result, status.getStatus());
        }

        shared_ptr<DBConfig> confIn = status.getValue();

        shared_ptr<DBConfig> confOut;
        if (customOutDB) {
            // Create the output database implicitly, since we have a custom output requested
            confOut = uassertStatusOK(grid.implicitCreateDb(outDB));
        } else {
            confOut = confIn;
        }

        const bool shardedInput =
            confIn && confIn->isShardingEnabled() && confIn->isSharded(fullns);
        const bool shardedOutput = customOut.getBoolField("sharded");

        if (!shardedOutput) {
            uassert(15920,
                    "Cannot output to a non-sharded collection because "
                    "sharded collection exists already",
                    !confOut->isSharded(finalColLong));

            // TODO: Should we also prevent going from non-sharded to sharded? During the
            //       transition client may see partial data.
        }

        int64_t maxChunkSizeBytes = 0;
        if (shardedOutput) {
            // Will need to figure out chunks, ask shards for points
            maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
            if (maxChunkSizeBytes == 0) {
                maxChunkSizeBytes = Chunk::MaxChunkSize;
            }

            // maxChunkSizeBytes is sent as int BSON field
            invariant(maxChunkSizeBytes < std::numeric_limits<int>::max());
        }

        if (customOut.hasField("inline") && shardedOutput) {
            errmsg = "cannot specify inline and sharded output at the same time";
            return false;
        }

        // modify command to run on shards with output to tmp collection
        string badShardedField;
        BSONObj shardedCommand =
            fixForShards(cmdObj, shardResultCollection, badShardedField, maxChunkSizeBytes);

        if (!shardedInput && !shardedOutput && !customOutDB) {
            LOG(1) << "simple MR, just passthrough";

            const auto shard = grid.shardRegistry()->getShard(confIn->getPrimaryId());
            ShardConnection conn(shard->getConnString(), "");

            BSONObj res;
            bool ok = conn->runCommand(dbname, cmdObj, res);
            conn.done();

            result.appendElements(res);
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
                Strategy::commandOp(dbname, shardedCommand, 0, fullns, q, &mrCommandResults);
            } catch (DBException& e) {
                e.addContext(str::stream() << "could not run map command on all shards for ns "
                                           << fullns << " and query " << q);
                throw;
            }

            for (const auto& mrResult : mrCommandResults) {
                // Need to gather list of all servers even if an error happened
                string server;
                {
                    const auto shard = grid.shardRegistry()->getShard(mrResult.shardTargetId);
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
                    errmsg = str::stream()
                        << "MR parallel processing failed: " << singleResult.toString();
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

        if (auto elem = cmdObj[LiteParsedQuery::cmdOptionMaxTimeMS])
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
            const auto shard = grid.shardRegistry()->getShard(confOut->getPrimaryId());
            LOG(1) << "MR with single shard output, NS=" << finalColLong
                   << " primary=" << shard->toString();

            ShardConnection conn(shard->getConnString(), finalColLong);
            ok = conn->runCommand(outDB, finalCmd.obj(), singleResult);

            BSONObj counts = singleResult.getObjectField("counts");
            postCountsB.append(conn->getServerAddress(), counts);
            reduceCount = counts.getIntField("reduce");
            outputCount = counts.getIntField("output");

            conn.done();
        } else {
            LOG(1) << "MR with sharded output, NS=" << finalColLong;

            // Create the sharded collection if needed
            if (!confOut->isSharded(finalColLong)) {
                // Enable sharding on db
                confOut->enableSharding();

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
                Status status = grid.catalogManager()->shardCollection(
                    finalColLong, sortKeyPattern, true, &sortedSplitPts, &outShardIds);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
            }

            map<BSONObj, int> chunkSizes;
            {
                // Take distributed lock to prevent split / migration.
                auto scopedDistLock = grid.catalogManager()->getDistLockManager()->lock(
                    finalColLong,
                    "mr-post-process",
                    stdx::chrono::milliseconds(-1),  // retry indefinitely
                    stdx::chrono::milliseconds(100));

                if (!scopedDistLock.isOK()) {
                    return appendCommandStatus(result, scopedDistLock.getStatus());
                }

                BSONObj finalCmdObj = finalCmd.obj();
                mrCommandResults.clear();

                try {
                    Strategy::commandOp(
                        outDB, finalCmdObj, 0, finalColLong, BSONObj(), &mrCommandResults);
                    ok = true;
                } catch (DBException& e) {
                    e.addContext(str::stream() << "could not run final reduce on all shards for "
                                               << fullns << ", output " << finalColLong);
                    throw;
                }

                for (const auto& mrResult : mrCommandResults) {
                    string server;
                    {
                        const auto shard = grid.shardRegistry()->getShard(mrResult.shardTargetId);
                        server = shard->getConnString().toString();
                    }
                    singleResult = mrResult.result;

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
            ChunkManagerPtr cm = confOut->getChunkManagerIfExists(finalColLong);
            for (const auto& chunkSize : chunkSizes) {
                BSONObj key = chunkSize.first;
                const int size = chunkSize.second;
                invariant(size < std::numeric_limits<int>::max());

                // key reported should be the chunk's minimum
                ChunkPtr c = cm->findIntersectingChunk(key);
                if (!c) {
                    warning() << "Mongod reported " << size << " bytes inserted for key " << key
                              << " but can't find chunk";
                } else {
                    c->splitIfShould(size);
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
