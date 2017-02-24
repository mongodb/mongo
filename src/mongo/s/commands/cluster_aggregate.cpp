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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_aggregate.h"

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_query_knobs.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

Status ClusterAggregate::runAggregate(OperationContext* txn,
                                      const Namespaces& namespaces,
                                      BSONObj cmdObj,
                                      int options,
                                      BSONObjBuilder* result) {
    auto scopedShardDbStatus = ScopedShardDatabase::getExisting(txn, namespaces.executionNss.db());
    if (!scopedShardDbStatus.isOK()) {
        appendEmptyResultSet(
            *result, scopedShardDbStatus.getStatus(), namespaces.requestedNss.ns());
        return Status::OK();
    }

    auto request = AggregationRequest::parseFromBSON(namespaces.executionNss, cmdObj);
    if (!request.isOK()) {
        return request.getStatus();
    }

    const auto conf = scopedShardDbStatus.getValue().db();

    // Determine the appropriate collation and 'resolve' involved namespaces to make the
    // ExpressionContext.

    // We won't try to execute anything on a mongos, but we still have to populate this map so that
    // any $lookups, etc. will be able to have a resolved view definition. It's okay that this is
    // incorrect, we will repopulate the real resolved namespace map on the mongod. Note that we
    // need to check if any involved collections are sharded before forwarding an aggregation
    // command on an unsharded collection.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    LiteParsedPipeline liteParsedPipeline(request.getValue());
    for (auto&& ns : liteParsedPipeline.getInvolvedNamespaces()) {
        uassert(28769, str::stream() << ns.ns() << " cannot be sharded", !conf->isSharded(ns.ns()));
        resolvedNamespaces[ns.coll()] = {ns, std::vector<BSONObj>{}};
    }

    if (!conf->isSharded(namespaces.executionNss.ns())) {
        return aggPassthrough(txn, namespaces, conf, cmdObj, result, options);
    }

    auto chunkMgr = conf->getChunkManager(txn, namespaces.executionNss.ns());

    std::unique_ptr<CollatorInterface> collation;
    if (!request.getValue().getCollation().isEmpty()) {
        collation = uassertStatusOK(CollatorFactoryInterface::get(txn->getServiceContext())
                                        ->makeFromBSON(request.getValue().getCollation()));
    } else if (chunkMgr->getDefaultCollator()) {
        collation = chunkMgr->getDefaultCollator()->clone();
    }

    boost::intrusive_ptr<ExpressionContext> mergeCtx = new ExpressionContext(
        txn, request.getValue(), std::move(collation), std::move(resolvedNamespaces));
    mergeCtx->inRouter = true;
    // explicitly *not* setting mergeCtx->tempDir

    // Parse and optimize the pipeline specification.
    auto pipeline = Pipeline::parse(request.getValue().getPipeline(), mergeCtx);
    if (!pipeline.isOK()) {
        return pipeline.getStatus();
    }

    pipeline.getValue()->optimizePipeline();

    // If the first $match stage is an exact match on the shard key (with a simple collation or no
    // string matching), we only have to send it to one shard, so send the command to that shard.
    const bool singleShard = [&]() {
        BSONObj firstMatchQuery = pipeline.getValue()->getInitialQuery();
        BSONObj shardKeyMatches = uassertStatusOK(
            chunkMgr->getShardKeyPattern().extractShardKeyFromQuery(txn, firstMatchQuery));

        if (shardKeyMatches.isEmpty()) {
            return false;
        }

        try {
            chunkMgr->findIntersectingChunk(shardKeyMatches, request.getValue().getCollation());
            return true;
        } catch (const DBException&) {
            return false;
        }
    }();

    // Don't need to split pipeline if the first $match is an exact match on shard key, unless
    // there is a stage that needs to be run on the primary shard.
    const bool needPrimaryShardMerger = pipeline.getValue()->needsPrimaryShardMerger();
    const bool needSplit = !singleShard || needPrimaryShardMerger;

    // Split the pipeline into pieces for mongod(s) and this mongos. If needSplit is true,
    // 'pipeline' will become the merger side.
    boost::intrusive_ptr<Pipeline> shardPipeline(needSplit ? pipeline.getValue()->splitForSharded()
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
    std::vector<Strategy::CommandResult> shardResults;
    Strategy::commandOp(txn,
                        namespaces.executionNss.db().toString(),
                        shardedCommand,
                        options,
                        namespaces.executionNss.ns(),
                        shardQuery,
                        request.getValue().getCollation(),
                        &shardResults);

    if (mergeCtx->isExplain) {
        // This must be checked before we start modifying result.
        uassertAllShardsSupportExplain(shardResults);

        if (needSplit) {
            *result << "needsPrimaryShardMerger" << needPrimaryShardMerger << "splitPipeline"
                    << DOC("shardsPart" << shardPipeline->writeExplainOps() << "mergerPart"
                                        << pipeline.getValue()->writeExplainOps());
        } else {
            *result << "splitPipeline" << BSONNULL;
        }

        BSONObjBuilder shardExplains(result->subobjStart("shards"));
        for (size_t i = 0; i < shardResults.size(); i++) {
            shardExplains.append(shardResults[i].shardTargetId,
                                 BSON("host" << shardResults[i].target.toString() << "stages"
                                             << shardResults[i].result["stages"]));
        }

        return Status::OK();
    }

    if (!needSplit) {
        invariant(shardResults.size() == 1);
        invariant(shardResults[0].target.getServers().size() == 1);
        auto executorPool = Grid::get(txn)->getExecutorPool();
        const BSONObj reply =
            uassertStatusOK(storePossibleCursor(shardResults[0].target.getServers()[0],
                                                shardResults[0].result,
                                                namespaces.requestedNss,
                                                executorPool->getArbitraryExecutor(),
                                                Grid::get(txn)->getCursorManager()));
        result->appendElements(reply);
        return getStatusFromCommandResult(reply);
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
    mergeCmd.setField("readConcern", Value(cmdObj["readConcern"]));

    // If the user didn't specify a collation already, make sure there's a collation attached to
    // the merge command, since the merging shard may not have the collection metadata.
    if (mergeCmd.peek()["collation"].missing()) {
        mergeCmd.setField("collation",
                          mergeCtx->getCollator()
                              ? Value(mergeCtx->getCollator()->getSpec().toBSON())
                              : Value(Document{CollationSpec::kSimpleSpec}));
    }

    std::string outputNsOrEmpty;
    if (DocumentSourceOut* out =
            dynamic_cast<DocumentSourceOut*>(pipeline.getValue()->getSources().back().get())) {
        outputNsOrEmpty = out->getOutputNs().ns();
    }

    // Run merging command on random shard, unless a stage needs the primary shard. Need to use
    // ShardConnection so that the merging mongod is sent the config servers on connection init.
    auto& prng = txn->getClient()->getPrng();
    const auto& mergingShardId = (needPrimaryShardMerger || internalQueryAlwaysMergeOnPrimaryShard)
        ? conf->getPrimaryId()
        : shardResults[prng.nextInt32(shardResults.size())].shardTargetId;
    const auto mergingShard =
        uassertStatusOK(Grid::get(txn)->shardRegistry()->getShard(txn, mergingShardId));

    ShardConnection conn(mergingShard->getConnString(), outputNsOrEmpty);
    BSONObj mergedResults =
        aggRunCommand(txn, conn.get(), namespaces, mergeCmd.freeze().toBson(), options);
    conn.done();

    if (auto wcErrorElem = mergedResults["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(mergingShardId, wcErrorElem, *result);
    }

    // Copy output from merging (primary) shard to the output object from our command.
    // Also, propagates errmsg and code if ok == false.
    result->appendElementsUnique(mergedResults);

    return getStatusFromCommandResult(result->asTempObj());
}

std::vector<DocumentSourceMergeCursors::CursorDescriptor> ClusterAggregate::parseCursors(
    const std::vector<Strategy::CommandResult>& shardResults) {
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

void ClusterAggregate::uassertAllShardsSupportExplain(
    const std::vector<Strategy::CommandResult>& shardResults) {
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

void ClusterAggregate::killAllCursors(const std::vector<Strategy::CommandResult>& shardResults) {
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
                  << " due to DBException: " << redact(e);
        } catch (const std::exception& e) {
            log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                  << " due to std::exception: " << redact(e.what());
        } catch (...) {
            log() << "Couldn't kill aggregation cursor on shard: " << shardResults[i].target
                  << " due to non-exception";
        }
    }
}

BSONObj ClusterAggregate::aggRunCommand(OperationContext* txn,
                                        DBClientBase* conn,
                                        const Namespaces& namespaces,
                                        BSONObj cmd,
                                        int queryOptions) {
    // Temporary hack. See comment on declaration for details.

    massert(17016,
            "should only be running an aggregate command here",
            str::equals(cmd.firstElementFieldName(), "aggregate"));

    auto cursor = conn->query(namespaces.executionNss.db() + ".$cmd",
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

    auto executorPool = Grid::get(txn)->getExecutorPool();
    result = uassertStatusOK(storePossibleCursor(HostAndPort(cursor->originalHost()),
                                                 result,
                                                 namespaces.requestedNss,
                                                 executorPool->getArbitraryExecutor(),
                                                 Grid::get(txn)->getCursorManager()));
    return result;
}

Status ClusterAggregate::aggPassthrough(OperationContext* txn,
                                        const Namespaces& namespaces,
                                        DBConfig* conf,
                                        BSONObj cmdObj,
                                        BSONObjBuilder* out,
                                        int queryOptions) {
    // Temporary hack. See comment on declaration for details.
    auto shardStatus = Grid::get(txn)->shardRegistry()->getShard(txn, conf->getPrimaryId());
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }

    ShardConnection conn(shardStatus.getValue()->getConnString(), "");
    BSONObj result = aggRunCommand(txn, conn.get(), namespaces, cmdObj, queryOptions);
    conn.done();

    // First append the properly constructed writeConcernError. It will then be skipped
    // in appendElementsUnique.
    if (auto wcErrorElem = result["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shardStatus.getValue()->getId(), wcErrorElem, *out);
    }

    out->appendElementsUnique(result);

    BSONObj responseObj = out->asTempObj();
    if (ResolvedView::isResolvedViewErrorResponse(responseObj)) {
        auto resolvedView = ResolvedView::fromBSON(responseObj);

        auto request = AggregationRequest::parseFromBSON(resolvedView.getNamespace(), cmdObj);
        if (!request.isOK()) {
            out->resetToEmpty();
            return request.getStatus();
        }

        auto aggCmd = resolvedView.asExpandedViewAggregation(request.getValue());
        if (!aggCmd.isOK()) {
            out->resetToEmpty();
            return aggCmd.getStatus();
        }

        out->resetToEmpty();

        // We pass both the underlying collection namespace and the view namespace here. The
        // underlying collection namespace is used to execute the aggregation on mongoD. Any cursor
        // returned will be registered under the view namespace so that subsequent getMore and
        // killCursors calls against the view have access.
        Namespaces nsStruct;
        nsStruct.requestedNss = namespaces.requestedNss;
        nsStruct.executionNss = resolvedView.getNamespace();
        return ClusterAggregate::runAggregate(txn, nsStruct, aggCmd.getValue(), queryOptions, out);
    }

    return getStatusFromCommandResult(result);
}

}  // namespace mongo
