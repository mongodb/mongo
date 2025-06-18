/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/s/commands/query_cmd/cluster_explain.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <fmt/format.h>

namespace mongo {

using std::vector;

const char* ClusterExplain::kSingleShard = "SINGLE_SHARD";
const char* ClusterExplain::kMergeFromShards = "SHARD_MERGE";
const char* ClusterExplain::kMergeSortFromShards = "SHARD_MERGE_SORT";
const char* ClusterExplain::kWriteOnShards = "SHARD_WRITE";

namespace {

//
// BSON size limit management: these functions conditionally append to a
// BSON object or BSON array buidlder, depending on whether or not the
// maximum user size for a BSON object will be exceeded.
//

bool appendIfRoom(BSONObjBuilder* bob, const BSONObj& toAppend, StringData fieldName) {
    if ((bob->len() + toAppend.objsize()) < BSONObjMaxUserSize) {
        bob->append(fieldName, toAppend);
        return true;
    }

    // Unless 'bob' has already exceeded the max BSON user size, add a warning indicating
    // that data has been truncated.
    if (bob->len() < BSONObjMaxUserSize) {
        bob->append("warning", "output truncated due to nearing BSON max user size");
    }

    return false;
}

bool appendToArrayIfRoom(BSONArrayBuilder* arrayBuilder, const BSONElement& toAppend) {
    if ((arrayBuilder->len() + toAppend.size()) < BSONObjMaxUserSize) {
        arrayBuilder->append(toAppend);
        return true;
    }

    // Unless 'arrayBuilder' has already exceeded the max BSON user size, add a warning
    // indicating that data has been truncated.
    if (arrayBuilder->len() < BSONObjMaxUserSize) {
        arrayBuilder->append(
            BSON("warning" << "output truncated due to nearing BSON max user size"));
    }

    return false;
}

bool appendElementsIfRoom(BSONObjBuilder* bob, const BSONObj& toAppend) {
    if ((bob->len() + toAppend.objsize()) < BSONObjMaxUserSize) {
        bob->appendElements(toAppend);
        return true;
    }

    // Unless 'bob' has already exceeded the max BSON user size, add a warning indicating
    // that data has been truncated.
    if (bob->len() < BSONObjMaxUserSize) {
        bob->append("warning", "output truncated due to nearing BSON max user size");
    }

    return false;
}

void throwOnBadARSResponse(const AsyncRequestsSender::Response& arsResponse) {
    auto status = arsResponse.swResponse.isOK()
        ? getStatusFromCommandResult(arsResponse.swResponse.getValue().data)
        : arsResponse.swResponse.getStatus();
    uassertStatusOKWithContext(
        status, str::stream() << "Explain command on shard " << arsResponse.shardId << " failed");

    invariant(arsResponse.shardHostAndPort);
}

}  // namespace

// static
BSONObj ClusterExplain::wrapAsExplain(const BSONObj& cmdObj,
                                      ExplainOptions::Verbosity verbosity,
                                      const BSONObj& querySettings) {
    BSONObjBuilder out;
    // Prune generic arguments out of the inner command since any relevant ones should already
    // be provided to the outer explain command. The shards will only process them at the top level.
    // As an exception, the "comment" parameter will be propagated out of the inner command to
    // maintain the behavior in our documentation:
    // https://www.mongodb.com/docs/manual/reference/command/explain/.
    // The "readConcern" parameter will also be propagated out of the inner command as the final
    // explain command inherits readConcern from the inner command invocation.
    // Another exception is the "rawData" field, which must be passed along in the inner command.
    BSONObjBuilder explainBuilder = out.subobjStart("explain");
    BSONElement commentField;
    BSONElement readConcernField;
    for (auto&& elem : cmdObj) {
        const auto& fieldName = elem.fieldNameStringData();
        if (!isGenericArgument(fieldName) || fieldName == kRawDataFieldName) {
            explainBuilder.append(elem);
        } else if (fieldName == "comment"_sd) {
            commentField = elem;
        } else if (fieldName == "readConcern"_sd) {
            readConcernField = elem;
        }
    }
    // Propagate query settings if there are any.
    if (!querySettings.isEmpty()) {
        explainBuilder.append("querySettings", querySettings);
    }
    explainBuilder.done();

    out.append("verbosity", ExplainOptions::verbosityString(verbosity));
    if (commentField) {
        out.append(commentField);
    }
    if (readConcernField) {
        out.append(readConcernField);
    }

    return out.obj();
}

// static
void ClusterExplain::validateShardResponses(
    const vector<AsyncRequestsSender::Response>& shardResponses) {
    // Error we didn't get responses from any shards.
    uassert(ErrorCodes::InternalError, "No shards found for explain", !shardResponses.empty());

    // Count up the number of shards that have execution stats and all plan execution level
    // information.
    size_t numShardsQueryPlanner = 0;
    size_t numShardsExecStats = 0;
    size_t numShardsAllPlansStats = 0;

    // Check that the response from each shard has a true value for "ok" and has
    // the expected "queryPlanner" field.
    for (auto&& response : shardResponses) {
        throwOnBadARSResponse(response);

        auto responseData = response.swResponse.getValue().data;

        if (const auto& shardQueryPlanner = responseData["queryPlanner"]) {
            uassert(ErrorCodes::OperationFailed,
                    str::stream() << "Explain command on shard " << response.shardId
                                  << " failed, caused by: " << responseData,
                    shardQueryPlanner.type() == BSONType::object);
            numShardsQueryPlanner++;
        } else {
            uassert(ErrorCodes::OperationFailed,
                    fmt::format("Explain command response from shard '{}' does not contain neither "
                                "'queryPlanner' field nor 'stages' field. Response: {}",
                                response.shardId.toString(),
                                responseData.toString()),
                    responseData.hasField("stages"));
        }

        if (responseData.hasField("executionStats")) {
            numShardsExecStats++;
            BSONObj execStats = responseData["executionStats"].Obj();
            if (execStats.hasField("allPlansExecution")) {
                numShardsAllPlansStats++;
            }
        }
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "Only " << numShardsQueryPlanner << "/" << shardResponses.size()
                          << " had top level queryPlanner explain information.",
            numShardsQueryPlanner == 0 || numShardsQueryPlanner == shardResponses.size());

    // Either all shards should have execution stats info, or none should.
    uassert(ErrorCodes::InternalError,
            str::stream() << "Only " << numShardsExecStats << "/" << shardResponses.size()
                          << " had executionStats explain information.",
            numShardsExecStats == 0 || numShardsExecStats == shardResponses.size());

    // Either all shards should have all plans execution stats, or none should.
    uassert(ErrorCodes::InternalError,
            str::stream() << "Only " << numShardsAllPlansStats << "/" << shardResponses.size()
                          << " had allPlansExecution explain information.",
            numShardsAllPlansStats == 0 || numShardsAllPlansStats == shardResponses.size());
}

// static
const char* ClusterExplain::getStageNameForReadOp(size_t numShards, const BSONObj& explainObj) {
    if (numShards == 1) {
        return kSingleShard;
    } else if (explainObj.hasField("sort")) {
        return kMergeSortFromShards;
    } else {
        return kMergeFromShards;
    }
}

// static
void ClusterExplain::buildPlannerInfo(OperationContext* opCtx,
                                      const vector<AsyncRequestsSender::Response>& shardResponses,
                                      const char* mongosStageName,
                                      BSONObjBuilder* out) {
    const auto& firstShardResponseData = shardResponses[0].swResponse.getValue().data;

    if (firstShardResponseData.hasField("stages")) {
        // Explain output is from an aggregation execution.
        // Use the aggregation helper to merge shards output into the upstream response.
        sharded_agg_helpers::mergeExplainOutputFromShards(shardResponses, out);
        return;
    }

    BSONObjBuilder queryPlannerBob(out->subobjStart("queryPlanner"));

    BSONObjBuilder winningPlanBob(queryPlannerBob.subobjStart("winningPlan"));

    winningPlanBob.append("stage", mongosStageName);
    BSONArrayBuilder shardsBuilder(winningPlanBob.subarrayStart("shards"));
    for (size_t i = 0; i < shardResponses.size(); i++) {
        auto responseData = shardResponses[i].swResponse.getValue().data;

        BSONObjBuilder singleShardBob(shardsBuilder.subobjStart());

        BSONObj queryPlanner = responseData["queryPlanner"].Obj();
        BSONObj serverInfo = responseData["serverInfo"].Obj();

        singleShardBob.append("explainVersion", responseData["explainVersion"].valueStringData());
        singleShardBob.append("shardName", shardResponses[i].shardId.toString());
        {
            const auto shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardResponses[i].shardId));
            singleShardBob.append("connectionString", shard->getConnString().toString());
        }
        appendIfRoom(&singleShardBob, serverInfo, "serverInfo");
        appendElementsIfRoom(&singleShardBob, queryPlanner);

        singleShardBob.doneFast();
    }
    shardsBuilder.doneFast();
    winningPlanBob.doneFast();
    queryPlannerBob.doneFast();
}

// static
void ClusterExplain::buildExecStats(const vector<AsyncRequestsSender::Response>& shardResponses,
                                    const char* mongosStageName,
                                    long long millisElapsed,
                                    BSONObjBuilder* out,
                                    boost::optional<int64_t> limit,
                                    boost::optional<int64_t> skip) {
    auto firstShardResponseData = shardResponses[0].swResponse.getValue().data;
    if (!firstShardResponseData.hasField("executionStats")) {
        // The shards don't have execution stats info. Bail out without adding anything
        // to 'out'.
        return;
    }

    BSONObjBuilder executionStatsBob(out->subobjStart("executionStats"));

    // Collect summary stats from the shards.
    auto multipleShards = shardResponses.size() > 1;
    long long totalNReturned = 0;
    long long keysExamined = 0;
    long long docsExamined = 0;
    long long totalChildMillis = 0;
    long long totalNCounted = 0;
    auto appendNCounted = false;
    for (size_t i = 0; i < shardResponses.size(); i++) {
        auto responseData = shardResponses[i].swResponse.getValue().data;
        BSONObj execStats = responseData["executionStats"].Obj();
        if (execStats.hasField("nReturned")) {
            totalNReturned += execStats["nReturned"].numberLong();
        }
        if (execStats.hasField("totalKeysExamined")) {
            keysExamined += execStats["totalKeysExamined"].numberLong();
        }
        if (execStats.hasField("totalDocsExamined")) {
            docsExamined += execStats["totalDocsExamined"].numberLong();
        }
        if (execStats.hasField("executionTimeMillis")) {
            totalChildMillis += execStats["executionTimeMillis"].numberLong();
        }
        if (execStats.hasField("executionStages")) {
            BSONObj stage = execStats["executionStages"].Obj();
            if (stage.hasField("nCounted")) {
                totalNCounted += stage["nCounted"].numberLong();
                appendNCounted = true;
            }
        }
    }

    // Fill in top-level stats.
    long long finalNReturned = totalNReturned;
    // If the query has targeted multiple shards, the overall nReturned value should reflect the
    // final limit + skip that are applied router-side on the results returned from the shards. The
    // same is true for the nCounted value for the count command.
    if (multipleShards) {
        if (skip) {
            finalNReturned = std::max(finalNReturned - *skip, 0LL);
            totalNCounted = std::max(totalNCounted - *skip, 0LL);
        }

        if (limit) {
            finalNReturned = std::min(finalNReturned, static_cast<long long>(*limit));
            totalNCounted = std::min(totalNCounted, static_cast<long long>(*limit));
        }
    }

    executionStatsBob.appendNumber("nReturned", finalNReturned);
    executionStatsBob.appendNumber("executionTimeMillis", millisElapsed);
    executionStatsBob.appendNumber("totalKeysExamined", keysExamined);
    executionStatsBob.appendNumber("totalDocsExamined", docsExamined);

    // Fill in the tree of stages.
    BSONObjBuilder executionStagesBob(executionStatsBob.subobjStart("executionStages"));

    // Info for the root mongos stage.
    executionStagesBob.append("stage", mongosStageName);
    executionStatsBob.appendNumber("nReturned", finalNReturned);
    if (appendNCounted) {
        executionStatsBob.appendNumber("nCounted", totalNCounted);
    }
    if (multipleShards) {
        if (limit) {
            executionStatsBob.appendNumber("limitAmount", static_cast<long long>(*limit));
        }
        if (skip) {
            executionStatsBob.appendNumber("skipAmount", static_cast<long long>(*skip));
        }
    }
    executionStatsBob.appendNumber("executionTimeMillis", millisElapsed);
    executionStatsBob.appendNumber("totalKeysExamined", keysExamined);
    executionStatsBob.appendNumber("totalDocsExamined", docsExamined);
    executionStagesBob.append("totalChildMillis", totalChildMillis);

    BSONArrayBuilder execShardsBuilder(executionStagesBob.subarrayStart("shards"));
    for (size_t i = 0; i < shardResponses.size(); i++) {
        auto responseData = shardResponses[i].swResponse.getValue().data;

        BSONObjBuilder singleShardBob(execShardsBuilder.subobjStart());
        BSONObj execStats = responseData["executionStats"].Obj();

        singleShardBob.append("shardName", shardResponses[i].shardId.toString());
        appendElementsIfRoom(&singleShardBob, execStats);
        singleShardBob.doneFast();
    }

    execShardsBuilder.doneFast();
    executionStagesBob.doneFast();
    if (!firstShardResponseData["executionStats"].Obj().hasField("allPlansExecution")) {
        // The shards don't have execution stats for all plans, so we're done.
        executionStatsBob.doneFast();
        return;
    }

    // Add the allPlans stats from each shard.
    BSONArrayBuilder allPlansExecBob(executionStatsBob.subarrayStart("allPlansExecution"));
    for (size_t i = 0; i < shardResponses.size(); i++) {
        auto responseData = shardResponses[i].swResponse.getValue().data;

        BSONObjBuilder singleShardBob(allPlansExecBob.subobjStart());
        singleShardBob.append("shardName", shardResponses[i].shardId.toString());

        BSONObj execStats = responseData["executionStats"].Obj();
        vector<BSONElement> allPlans = execStats["allPlansExecution"].Array();

        BSONArrayBuilder innerArrayBob(singleShardBob.subarrayStart("allPlans"));
        for (size_t j = 0; j < allPlans.size(); j++) {
            appendToArrayIfRoom(&innerArrayBob, allPlans[j]);
        }
        innerArrayBob.done();

        singleShardBob.doneFast();
    }

    allPlansExecBob.doneFast();
    executionStatsBob.doneFast();
}

// static
void ClusterExplain::buildEOFExplainResult(OperationContext* opCtx,
                                           const CanonicalQuery* cq,
                                           const BSONObj& command,
                                           BSONObjBuilder* out) {

    BSONObjBuilder queryPlannerBob(out->subobjStart("queryPlanner"));
    queryPlannerBob.append(
        "namespace",
        NamespaceStringUtil::serialize(cq->nss(), SerializationContext::stateDefault()));

    BSONObjBuilder parsedQueryBob(queryPlannerBob.subobjStart("parsedQuery"));
    cq->getPrimaryMatchExpression()->serialize(&parsedQueryBob, {});
    parsedQueryBob.doneFast();

    BSONObjBuilder winningPlanBob(queryPlannerBob.subobjStart("winningPlan"));
    BSONObjBuilder queryPlanBob(winningPlanBob.subobjStart("queryPlan"));
    queryPlanBob.append("stage", "EOF");
    queryPlanBob.appendNumber("planNodeId", 1);
    queryPlanBob.append("type", eof_node::typeStr(eof_node::EOFType::NonExistentNamespace));
    queryPlanBob.doneFast();
    winningPlanBob.doneFast();

    queryPlannerBob.doneFast();

    explain_common::generateQueryShapeHash(opCtx, out);
    explain_common::generateServerInfo(out);
    explain_common::generateServerParameters(cq->getExpCtx(), out);
    appendIfRoom(out, command, "command");
}

// static
Status ClusterExplain::buildExplainResult(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const vector<AsyncRequestsSender::Response>& shardResponses,
    const char* mongosStageName,
    long long millisElapsed,
    const BSONObj& command,
    BSONObjBuilder* out,
    boost::optional<int64_t> limit,
    boost::optional<int64_t> skip) {
    // Explain only succeeds if all shards support the explain command.
    try {
        ClusterExplain::validateShardResponses(shardResponses);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    buildPlannerInfo(expCtx->getOperationContext(), shardResponses, mongosStageName, out);
    buildExecStats(shardResponses, mongosStageName, millisElapsed, out, limit, skip);
    explain_common::generateQueryShapeHash(expCtx->getOperationContext(), out);
    explain_common::generateServerInfo(out);
    explain_common::generateServerParameters(expCtx, out);
    appendIfRoom(out, command, "command");

    return Status::OK();
}


}  // namespace mongo
