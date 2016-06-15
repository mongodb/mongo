/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"

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
        arrayBuilder->append(BSON("warning"
                                  << "output truncated due to nearing BSON max user size"));
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

}  // namespace

// static
void ClusterExplain::wrapAsExplain(const BSONObj& cmdObj,
                                   ExplainCommon::Verbosity verbosity,
                                   const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                                   BSONObjBuilder* out,
                                   int* optionsOut) {
    BSONObjBuilder explainBuilder;
    explainBuilder.append("explain", cmdObj);
    explainBuilder.append("verbosity", ExplainCommon::verbosityString(verbosity));

    // Propagate readConcern
    if (auto readConcern = cmdObj["readConcern"]) {
        explainBuilder.append(readConcern);
    }

    const BSONObj explainCmdObj = explainBuilder.done();

    // Attach metadata to the explain command in legacy format.
    BSONObjBuilder metadataBuilder;
    serverSelectionMetadata.writeToMetadata(&metadataBuilder);
    const BSONObj metadataObj = metadataBuilder.done();
    uassertStatusOK(
        serverSelectionMetadata.downconvert(explainCmdObj, metadataObj, out, optionsOut));
}

// static
Status ClusterExplain::validateShardResults(const vector<Strategy::CommandResult>& shardResults) {
    // Error we didn't get results from any shards.
    if (shardResults.empty()) {
        return Status(ErrorCodes::InternalError, "no shards found for explain");
    }

    // Count up the number of shards that have execution stats and all plans
    // execution level information.
    size_t numShardsExecStats = 0;
    size_t numShardsAllPlansStats = 0;

    // Check that the result from each shard has a true value for "ok" and has
    // the expected "queryPlanner" field.
    for (size_t i = 0; i < shardResults.size(); i++) {
        if (!shardResults[i].result["ok"].trueValue()) {
            // Try to pass up the error code from the shard.
            ErrorCodes::Error error = ErrorCodes::OperationFailed;
            if (shardResults[i].result["code"].isNumber()) {
                error = ErrorCodes::fromInt(shardResults[i].result["code"].numberInt());
            }

            return Status(error,
                          str::stream() << "Explain command on shard "
                                        << shardResults[i].target.toString()
                                        << " failed, caused by: "
                                        << shardResults[i].result);
        }

        if (Object != shardResults[i].result["queryPlanner"].type()) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Explain command on shard "
                                        << shardResults[i].target.toString()
                                        << " failed, caused by: "
                                        << shardResults[i].result);
        }

        if (shardResults[i].result.hasField("executionStats")) {
            numShardsExecStats++;
            BSONObj execStats = shardResults[i].result["executionStats"].Obj();
            if (execStats.hasField("allPlansExecution")) {
                numShardsAllPlansStats++;
            }
        }
    }

    // Either all shards should have execution stats info, or none should.
    if (0 != numShardsExecStats && shardResults.size() != numShardsExecStats) {
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Only " << numShardsExecStats << "/" << shardResults.size()
                                    << " had executionStats explain information.");
    }

    // Either all shards should have all plans execution stats, or none should.
    if (0 != numShardsAllPlansStats && shardResults.size() != numShardsAllPlansStats) {
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Only " << numShardsAllPlansStats << "/"
                                    << shardResults.size()
                                    << " had allPlansExecution explain information.");
    }

    return Status::OK();
}

// static
const char* ClusterExplain::getStageNameForReadOp(
    const vector<Strategy::CommandResult>& shardResults, const BSONObj& explainObj) {
    if (shardResults.size() == 1) {
        return kSingleShard;
    } else if (explainObj.hasField("sort")) {
        return kMergeSortFromShards;
    } else {
        return kMergeFromShards;
    }
}

// static
void ClusterExplain::buildPlannerInfo(OperationContext* txn,
                                      const vector<Strategy::CommandResult>& shardResults,
                                      const char* mongosStageName,
                                      BSONObjBuilder* out) {
    BSONObjBuilder queryPlannerBob(out->subobjStart("queryPlanner"));

    queryPlannerBob.appendNumber("mongosPlannerVersion", 1);
    BSONObjBuilder winningPlanBob(queryPlannerBob.subobjStart("winningPlan"));

    winningPlanBob.append("stage", mongosStageName);
    BSONArrayBuilder shardsBuilder(winningPlanBob.subarrayStart("shards"));
    for (size_t i = 0; i < shardResults.size(); i++) {
        BSONObjBuilder singleShardBob(shardsBuilder.subobjStart());

        BSONObj queryPlanner = shardResults[i].result["queryPlanner"].Obj();
        BSONObj serverInfo = shardResults[i].result["serverInfo"].Obj();

        singleShardBob.append("shardName", shardResults[i].shardTargetId.toString());
        {
            const auto shard = grid.shardRegistry()->getShard(txn, shardResults[i].shardTargetId);
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
void ClusterExplain::buildExecStats(const vector<Strategy::CommandResult>& shardResults,
                                    const char* mongosStageName,
                                    long long millisElapsed,
                                    BSONObjBuilder* out) {
    if (!shardResults[0].result.hasField("executionStats")) {
        // The shards don't have execution stats info. Bail out without adding anything
        // to 'out'.
        return;
    }

    BSONObjBuilder executionStatsBob(out->subobjStart("executionStats"));

    // Collect summary stats from the shards.
    long long nReturned = 0;
    long long keysExamined = 0;
    long long docsExamined = 0;
    long long totalChildMillis = 0;
    for (size_t i = 0; i < shardResults.size(); i++) {
        BSONObj execStats = shardResults[i].result["executionStats"].Obj();
        if (execStats.hasField("nReturned")) {
            nReturned += execStats["nReturned"].numberLong();
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
    }

    // Fill in top-level stats.
    executionStatsBob.appendNumber("nReturned", nReturned);
    executionStatsBob.appendNumber("executionTimeMillis", millisElapsed);
    executionStatsBob.appendNumber("totalKeysExamined", keysExamined);
    executionStatsBob.appendNumber("totalDocsExamined", docsExamined);

    // Fill in the tree of stages.
    BSONObjBuilder executionStagesBob(executionStatsBob.subobjStart("executionStages"));

    // Info for the root mongos stage.
    executionStagesBob.append("stage", mongosStageName);
    executionStatsBob.appendNumber("nReturned", nReturned);
    executionStatsBob.appendNumber("executionTimeMillis", millisElapsed);
    executionStatsBob.appendNumber("totalKeysExamined", keysExamined);
    executionStatsBob.appendNumber("totalDocsExamined", docsExamined);
    executionStagesBob.append("totalChildMillis", totalChildMillis);

    BSONArrayBuilder execShardsBuilder(executionStagesBob.subarrayStart("shards"));
    for (size_t i = 0; i < shardResults.size(); i++) {
        BSONObjBuilder singleShardBob(execShardsBuilder.subobjStart());

        BSONObj execStats = shardResults[i].result["executionStats"].Obj();
        BSONObj execStages = execStats["executionStages"].Obj();

        singleShardBob.append("shardName", shardResults[i].shardTargetId.toString());

        // Append error-related fields, if present.
        if (!execStats["executionSuccess"].eoo()) {
            singleShardBob.append(execStats["executionSuccess"]);
        }
        if (!execStats["errorMessage"].eoo()) {
            singleShardBob.append(execStats["errorMessage"]);
        }
        if (!execStats["errorCode"].eoo()) {
            singleShardBob.append(execStats["errorCode"]);
        }

        appendIfRoom(&singleShardBob, execStages, "executionStages");

        singleShardBob.doneFast();
    }
    execShardsBuilder.doneFast();

    executionStagesBob.doneFast();

    if (!shardResults[0].result["executionStats"].Obj().hasField("allPlansExecution")) {
        // The shards don't have execution stats for all plans, so we're done.
        executionStatsBob.doneFast();
        return;
    }

    // Add the allPlans stats from each shard.
    BSONArrayBuilder allPlansExecBob(executionStatsBob.subarrayStart("allPlansExecution"));
    for (size_t i = 0; i < shardResults.size(); i++) {
        BSONObjBuilder singleShardBob(execShardsBuilder.subobjStart());

        singleShardBob.append("shardName", shardResults[i].shardTargetId.toString());

        BSONObj execStats = shardResults[i].result["executionStats"].Obj();
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
Status ClusterExplain::buildExplainResult(OperationContext* txn,
                                          const vector<Strategy::CommandResult>& shardResults,
                                          const char* mongosStageName,
                                          long long millisElapsed,
                                          BSONObjBuilder* out) {
    // Explain only succeeds if all shards support the explain command.
    Status validateStatus = ClusterExplain::validateShardResults(shardResults);
    if (!validateStatus.isOK()) {
        return validateStatus;
    }

    buildPlannerInfo(txn, shardResults, mongosStageName, out);
    buildExecStats(shardResults, mongosStageName, millisElapsed, out);

    return Status::OK();
}

}  // namespace mongo
