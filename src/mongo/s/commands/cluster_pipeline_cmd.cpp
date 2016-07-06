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

#include <boost/intrusive_ptr.hpp>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::unique_ptr;
using std::shared_ptr;
using std::string;
using std::vector;

namespace {

/**
 * Implements the aggregation (pipeline command for sharding).
 */
class PipelineCommand : public Command {
public:
    PipelineCommand() : Command(AggregationRequest::kCommandName, false) {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return Pipeline::aggSupportsWriteConcern(cmd);
    }

    virtual void help(std::stringstream& help) const {
        help << "Runs the sharded aggregation command";
    }

    // virtuals from Command
    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        return Pipeline::checkAuthForCommand(client, dbname, cmdObj);
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const string fullns = parseNs(dbname, cmdObj);

        auto status = grid.catalogCache()->getDatabase(txn, dbname);
        if (!status.isOK()) {
            return appendEmptyResultSet(result, status.getStatus(), fullns);
        }

        shared_ptr<DBConfig> conf = status.getValue();

        if (!conf->isShardingEnabled()) {
            return aggPassthrough(txn, conf, cmdObj, result, options);
        }

        auto request = AggregationRequest::parseFromBSON(NamespaceString(fullns), cmdObj);
        if (!request.isOK()) {
            return appendCommandStatus(result, request.getStatus());
        }

        intrusive_ptr<ExpressionContext> mergeCtx = new ExpressionContext(txn, request.getValue());
        mergeCtx->inRouter = true;
        // explicitly *not* setting mergeCtx->tempDir

        // Parse and optimize the pipeline specification.
        auto pipeline = Pipeline::parse(request.getValue().getPipeline(), mergeCtx);
        if (!pipeline.isOK()) {
            return appendCommandStatus(result, pipeline.getStatus());
        }
        pipeline.getValue()->optimizePipeline();

        for (auto&& ns : pipeline.getValue()->getInvolvedCollections()) {
            uassert(
                28769, str::stream() << ns.ns() << " cannot be sharded", !conf->isSharded(ns.ns()));
        }

        if (!conf->isSharded(fullns)) {
            return aggPassthrough(txn, conf, cmdObj, result, options);
        }

        // If the first $match stage is an exact match on the shard key, we only have to send it
        // to one shard, so send the command to that shard.
        BSONObj firstMatchQuery = pipeline.getValue()->getInitialQuery();
        ChunkManagerPtr chunkMgr = conf->getChunkManager(txn, fullns);
        BSONObj shardKeyMatches = uassertStatusOK(
            chunkMgr->getShardKeyPattern().extractShardKeyFromQuery(txn, firstMatchQuery));

        // Don't need to split pipeline if the first $match is an exact match on shard key, unless
        // there is a stage that needs to be run on the primary shard.
        const bool needPrimaryShardMerger = pipeline.getValue()->needsPrimaryShardMerger();
        const bool needSplit = shardKeyMatches.isEmpty() || needPrimaryShardMerger;

        // Split the pipeline into pieces for mongod(s) and this mongos. If needSplit is true,
        // 'pipeline' will become the merger side.
        intrusive_ptr<Pipeline> shardPipeline(needSplit ? pipeline.getValue()->splitForSharded()
                                                        : pipeline.getValue());

        // Create the command for the shards. The 'fromRouter' field means produce output to be
        // merged.
        MutableDocument commandBuilder(request.getValue().serializeToCommandObj());
        commandBuilder[AggregationRequest::kPipelineName] = Value(shardPipeline->serialize());
        if (needSplit) {
            commandBuilder[AggregationRequest::kFromRouterName] = Value(true);
            commandBuilder[AggregationRequest::kCursorName] =
                Value(DOC(AggregationRequest::kBatchSizeName << 0));
        }

        // These fields are not part of the AggregationRequest since they are not handled by the
        // aggregation subsystem, so we serialize them separately.
        const std::initializer_list<StringData> fieldsToPropagateToShards = {
            "$queryOptions", "readConcern", QueryRequest::cmdOptionMaxTimeMS,
        };
        for (auto&& field : fieldsToPropagateToShards) {
            commandBuilder[field] = Value(cmdObj[field]);
        }

        BSONObj shardedCommand = commandBuilder.freeze().toBson();
        BSONObj shardQuery = shardPipeline->getInitialQuery();

        // Run the command on the shards
        // TODO need to make sure cursors are killed if a retry is needed
        vector<Strategy::CommandResult> shardResults;
        Strategy::commandOp(
            txn, dbname, shardedCommand, options, fullns, shardQuery, &shardResults);

        if (mergeCtx->isExplain) {
            // This must be checked before we start modifying result.
            uassertAllShardsSupportExplain(shardResults);

            if (needSplit) {
                result << "needsPrimaryShardMerger" << needPrimaryShardMerger << "splitPipeline"
                       << DOC("shardsPart" << shardPipeline->writeExplainOps() << "mergerPart"
                                           << pipeline.getValue()->writeExplainOps());
            } else {
                result << "splitPipeline" << BSONNULL;
            }

            BSONObjBuilder shardExplains(result.subobjStart("shards"));
            for (size_t i = 0; i < shardResults.size(); i++) {
                shardExplains.append(shardResults[i].shardTargetId,
                                     BSON("host" << shardResults[i].target.toString() << "stages"
                                                 << shardResults[i].result["stages"]));
            }

            return true;
        }

        if (!needSplit) {
            invariant(shardResults.size() == 1);
            invariant(shardResults[0].target.getServers().size() == 1);
            auto executorPool = grid.getExecutorPool();
            const BSONObj reply =
                uassertStatusOK(storePossibleCursor(shardResults[0].target.getServers()[0],
                                                    shardResults[0].result,
                                                    executorPool->getArbitraryExecutor(),
                                                    grid.getCursorManager()));
            result.appendElements(reply);
            return reply["ok"].trueValue();
        }

        pipeline.getValue()->addInitialSource(
            DocumentSourceMergeCursors::create(parseCursors(shardResults), mergeCtx));

        MutableDocument mergeCmd(request.getValue().serializeToCommandObj());
        mergeCmd["pipeline"] = Value(pipeline.getValue()->serialize());
        mergeCmd["cursor"] = Value(cmdObj["cursor"]);

        if (cmdObj.hasField("$queryOptions")) {
            mergeCmd["$queryOptions"] = Value(cmdObj["$queryOptions"]);
        }

        if (cmdObj.hasField(QueryRequest::cmdOptionMaxTimeMS)) {
            mergeCmd[QueryRequest::cmdOptionMaxTimeMS] =
                Value(cmdObj[QueryRequest::cmdOptionMaxTimeMS]);
        }

        mergeCmd.setField("writeConcern", Value(cmdObj["writeConcern"]));

        // Not propagating readConcern to merger since it doesn't do local reads.

        string outputNsOrEmpty;
        if (DocumentSourceOut* out =
                dynamic_cast<DocumentSourceOut*>(pipeline.getValue()->output())) {
            outputNsOrEmpty = out->getOutputNs().ns();
        }

        // Run merging command on random shard, unless a stage needs the primary shard. Need to use
        // ShardConnection so that the merging mongod is sent the config servers on connection init.
        auto& prng = txn->getClient()->getPrng();
        const auto& mergingShardId = needPrimaryShardMerger
            ? conf->getPrimaryId()
            : shardResults[prng.nextInt32(shardResults.size())].shardTargetId;
        const auto mergingShard = grid.shardRegistry()->getShard(txn, mergingShardId);
        ShardConnection conn(mergingShard->getConnString(), outputNsOrEmpty);
        BSONObj mergedResults =
            aggRunCommand(conn.get(), dbname, mergeCmd.freeze().toBson(), options);
        conn.done();

        if (auto wcErrorElem = mergedResults["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(mergingShardId, wcErrorElem, result);
        }

        // Copy output from merging (primary) shard to the output object from our command.
        // Also, propagates errmsg and code if ok == false.
        result.appendElementsUnique(mergedResults);

        return mergedResults["ok"].trueValue();
    }

private:
    std::vector<DocumentSourceMergeCursors::CursorDescriptor> parseCursors(
        const vector<Strategy::CommandResult>& shardResults);

    void killAllCursors(const vector<Strategy::CommandResult>& shardResults);
    void uassertAllShardsSupportExplain(const vector<Strategy::CommandResult>& shardResults);

    // These are temporary hacks because the runCommand method doesn't report the exact
    // host the command was run on which is necessary for cursor support. The exact host
    // could be different from conn->getServerAddress() for connections that map to
    // multiple servers such as for replica sets. These also take care of registering
    // returned cursors.
    BSONObj aggRunCommand(DBClientBase* conn, const string& db, BSONObj cmd, int queryOptions);

    bool aggPassthrough(OperationContext* txn,
                        shared_ptr<DBConfig> conf,
                        BSONObj cmd,
                        BSONObjBuilder& result,
                        int queryOptions);
} clusterPipelineCmd;

std::vector<DocumentSourceMergeCursors::CursorDescriptor> PipelineCommand::parseCursors(
    const vector<Strategy::CommandResult>& shardResults) {
    try {
        std::vector<DocumentSourceMergeCursors::CursorDescriptor> cursors;

        for (size_t i = 0; i < shardResults.size(); i++) {
            BSONObj result = shardResults[i].result;

            if (!result["ok"].trueValue()) {
                // If the failure of the sharded command can be accounted to a single error,
                // throw a UserException with that error code; otherwise, throw with a
                // location uassert code.
                int errCode = getUniqueCodeFromCommandResults(shardResults);
                if (errCode == 0) {
                    errCode = 17022;
                }

                invariant(errCode == result["code"].numberInt() || errCode == 17022);
                uasserted(errCode,
                          str::stream() << "sharded pipeline failed on shard "
                                        << shardResults[i].shardTargetId
                                        << ": "
                                        << result.toString());
            }

            BSONObj cursor = result["cursor"].Obj();

            massert(17023,
                    str::stream() << "shard " << shardResults[i].shardTargetId
                                  << " returned non-empty first batch",
                    cursor["firstBatch"].Obj().isEmpty());

            massert(17024,
                    str::stream() << "shard " << shardResults[i].shardTargetId
                                  << " returned cursorId 0",
                    cursor["id"].Long() != 0);

            massert(17025,
                    str::stream() << "shard " << shardResults[i].shardTargetId
                                  << " returned invalid ns: "
                                  << cursor["ns"],
                    NamespaceString(cursor["ns"].String()).isValid());

            cursors.emplace_back(
                shardResults[i].target, cursor["ns"].String(), cursor["id"].Long());
        }

        return cursors;
    } catch (...) {
        // Need to clean up any cursors we successfully created on the shards
        killAllCursors(shardResults);
        throw;
    }
}

void PipelineCommand::uassertAllShardsSupportExplain(
    const vector<Strategy::CommandResult>& shardResults) {
    for (size_t i = 0; i < shardResults.size(); i++) {
        uassert(17403,
                str::stream() << "Shard " << shardResults[i].target.toString() << " failed: "
                              << shardResults[i].result,
                shardResults[i].result["ok"].trueValue());

        uassert(17404,
                str::stream() << "Shard " << shardResults[i].target.toString()
                              << " does not support $explain",
                shardResults[i].result.hasField("stages"));
    }
}

void PipelineCommand::killAllCursors(const vector<Strategy::CommandResult>& shardResults) {
    // This function must ignore and log all errors. Callers expect a best-effort attempt at
    // cleanup without exceptions. If any cursors aren't cleaned up here, they will be cleaned
    // up automatically on the shard after 10 minutes anyway.

    for (size_t i = 0; i < shardResults.size(); i++) {
        try {
            BSONObj result = shardResults[i].result;
            if (!result["ok"].trueValue()) {
                continue;
            }

            const long long cursor = result["cursor"]["id"].Long();
            if (!cursor) {
                continue;
            }

            ScopedDbConnection conn(shardResults[i].target);
            conn->killCursor(cursor);
            conn.done();
        } catch (const DBException& e) {
            log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                  << " due to DBException: " << e.toString();
        } catch (const std::exception& e) {
            log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                  << " due to std::exception: " << e.what();
        } catch (...) {
            log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                  << " due to non-exception";
        }
    }
}

BSONObj PipelineCommand::aggRunCommand(DBClientBase* conn,
                                       const string& db,
                                       BSONObj cmd,
                                       int queryOptions) {
    // Temporary hack. See comment on declaration for details.

    massert(17016,
            "should only be running an aggregate command here",
            str::equals(cmd.firstElementFieldName(), "aggregate"));

    auto cursor = conn->query(db + ".$cmd",
                              cmd,
                              -1,    // nToReturn
                              0,     // nToSkip
                              NULL,  // fieldsToReturn
                              queryOptions);
    massert(17014,
            str::stream() << "aggregate command didn't return results on host: "
                          << conn->toString(),
            cursor && cursor->more());

    BSONObj result = cursor->nextSafe().getOwned();

    if (ErrorCodes::SendStaleConfig == getStatusFromCommandResult(result)) {
        throw RecvStaleConfigException("command failed because of stale config", result);
    }

    auto executorPool = grid.getExecutorPool();
    result = uassertStatusOK(storePossibleCursor(HostAndPort(cursor->originalHost()),
                                                 result,
                                                 executorPool->getArbitraryExecutor(),
                                                 grid.getCursorManager()));
    return result;
}

bool PipelineCommand::aggPassthrough(OperationContext* txn,
                                     shared_ptr<DBConfig> conf,
                                     BSONObj cmd,
                                     BSONObjBuilder& out,
                                     int queryOptions) {
    // Temporary hack. See comment on declaration for details.
    const auto shard = grid.shardRegistry()->getShard(txn, conf->getPrimaryId());
    ShardConnection conn(shard->getConnString(), "");
    BSONObj result = aggRunCommand(conn.get(), conf->name(), cmd, queryOptions);
    conn.done();

    // First append the properly constructed writeConcernError. It will then be skipped
    // in appendElementsUnique.
    if (auto wcErrorElem = result["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shard->getId(), wcErrorElem, out);
    }

    out.appendElementsUnique(result);
    return result["ok"].trueValue();
}

}  // namespace
}  // namespace mongo
