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

#include "mongo/s/cluster_explain.h"

#include "mongo/bson/bsonmisc.h"

namespace mongo {

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

        bool appendIfRoom(BSONObjBuilder* bob,
                          const BSONObj& toAppend,
                          const StringData& fieldName) {
            if ((bob->len() + toAppend.objsize()) < BSONObjMaxUserSize) {
                bob->append(fieldName, toAppend);
                return true;
            }

            // Unless 'bob' has already exceeded the max BSON user size, add a warning indicating
            // that data has been truncated.
            if (bob->len() < BSONObjMaxUserSize) {
                bob->append("warning",
                            "output truncated due to nearing BSON max user size");
            }

            return false;
        }

        bool appendToArrayIfRoom(BSONArrayBuilder* arrayBuilder,
                                 const BSONElement& toAppend) {
            if ((arrayBuilder->len() + toAppend.size()) < BSONObjMaxUserSize) {
                arrayBuilder->append(toAppend);
                return true;
            }

            // Unless 'arrayBuilder' has already exceeded the max BSON user size, add a warning
            // indicating that data has been truncated.
            if (arrayBuilder->len() < BSONObjMaxUserSize) {
                arrayBuilder->append(BSON("warning" <<
                                          "output truncated due to nearing BSON max user size"));
            }

            return false;
        }

        bool appendElementsIfRoom(BSONObjBuilder* bob,
                                  const BSONObj& toAppend) {
            if ((bob->len() + toAppend.objsize()) < BSONObjMaxUserSize) {
                bob->appendElements(toAppend);
                return true;
            }

            // Unless 'bob' has already exceeded the max BSON user size, add a warning indicating
            // that data has been truncated.
            if (bob->len() < BSONObjMaxUserSize) {
                bob->append("warning",
                            "output truncated due to nearing BSON max user size");
            }

            return false;
        }

    } // namespace

    // static
    void ClusterExplain::wrapAsExplain(const BSONObj& cmdObj,
                                       ExplainCommon::Verbosity verbosity,
                                       BSONObjBuilder* out) {
        out->append("explain", cmdObj);
        out->append("verbosity", ExplainCommon::verbosityString(verbosity));

        // If the command has a readPreference, then pull it up to the top level.
        if (cmdObj.hasField("$readPreference")) {
            out->append("$queryOptions", cmdObj["$readPreference"].wrap());
        }
    }

    // static
    Status ClusterExplain::validateShardResults(
            const vector<Strategy::CommandResult>& shardResults) {
        // Error we didn't get results from any shards.
        if (shardResults.empty()) {
           return Status(ErrorCodes::InternalError, "no shards found for explain");
        }

        // Count up the number of shards that have execution stats and rejected plans
        // execution level information.
        size_t numShardsExecStats = 0;
        size_t numShardsRejectedExecStats = 0;

        // Check that the result from each shard has a true value for "ok" and has
        // the expected "queryPlanner" field.
        for (size_t i = 0; i < shardResults.size(); i++) {
            if (!shardResults[i].result["ok"].trueValue() ||
                Object != shardResults[i].result["queryPlanner"].type()) {
                return Status(ErrorCodes::OperationFailed,
                              str::stream() << "Shard " << shardResults[i].target.toString()
                                            << " failed: " << shardResults[i].result);
            }

            if (shardResults[i].result.hasField("executionStats")) {
                numShardsExecStats++;
                BSONObj execStats = shardResults[i].result["executionStats"].Obj();
                if (execStats.hasField("rejectedPlansExecution")) {
                    numShardsRejectedExecStats++;
                }
            }
        }

        // Either all shards should have execution stats info, or none should.
        if (0 != numShardsExecStats && shardResults.size() != numShardsExecStats) {
            return Status(ErrorCodes::InternalError,
                          str::stream() << "Only " << numShardsExecStats
                                        << "/" << shardResults.size()
                                        << " had executionStats explain information.");
        }

        // Either all shards should have rejected plans execution stats, or none should.
        if (0 != numShardsRejectedExecStats && shardResults.size() != numShardsRejectedExecStats) {
            return Status(ErrorCodes::InternalError,
                          str::stream() << "Only " << numShardsRejectedExecStats
                                        << "/" << shardResults.size()
                                        << " had allPlansExecution explain information.");
        }

        return Status::OK();
    }

    // static
    const char* ClusterExplain::getStageNameForReadOp(
            const vector<Strategy::CommandResult>& shardResults,
            const BSONObj& explainObj) {
        if (shardResults.size() == 1) {
            return kSingleShard;
        }
        else if (explainObj.hasField("sort")) {
            return kMergeSortFromShards;
        }
        else {
            return kMergeFromShards;
        }
    }

    // static
    void ClusterExplain::buildPlannerInfo(const vector<Strategy::CommandResult>& shardResults,
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

            singleShardBob.append("shardName", shardResults[i].shardTarget.getName());
            std::string connStr = shardResults[i].shardTarget.getAddress().toString();
            singleShardBob.append("connectionString", connStr);
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
        BSONObjBuilder executionStagesBob(
            executionStatsBob.subobjStart("executionStages"));

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

            singleShardBob.append("shardName", shardResults[i].shardTarget.getName());
            appendIfRoom(&singleShardBob, execStages, "executionStages");

            singleShardBob.doneFast();
        }
        execShardsBuilder.doneFast();

        executionStagesBob.doneFast();

        if (!shardResults[0].result["executionStats"].Obj().hasField("rejectedPlansExecution")) {
            // The shards don't have execution stats for rejected plans, so we're done.
            executionStatsBob.doneFast();
            return;
        }

        // Add the rejected plans from each shard.
        BSONArrayBuilder rejectedPlansExecBob(
            executionStatsBob.subarrayStart("rejectedPlansExecution"));
        for (size_t i = 0; i < shardResults.size(); i++) {
            BSONObjBuilder singleShardBob(execShardsBuilder.subobjStart());

            singleShardBob.append("shardName", shardResults[i].shardTarget.getName());

            BSONObj execStats = shardResults[i].result["executionStats"].Obj();
            vector<BSONElement> rejectedPlans = execStats["rejectedPlansExecution"].Array();

            BSONArrayBuilder innerArrayBob(singleShardBob.subarrayStart("rejectedPlans"));
            for (size_t j = 0; j < rejectedPlans.size(); j++) {
                appendToArrayIfRoom(&innerArrayBob, rejectedPlans[j]);
            }
            innerArrayBob.done();

            singleShardBob.doneFast();
        }
        rejectedPlansExecBob.doneFast();

        executionStatsBob.doneFast();
    }

    // static
    Status ClusterExplain::buildExplainResult(const vector<Strategy::CommandResult>& shardResults,
                                              const char* mongosStageName,
                                              long long millisElapsed,
                                              BSONObjBuilder* out) {
        // Explain only succeeds if all shards support the explain command.
        Status validateStatus = ClusterExplain::validateShardResults(shardResults);
        if (!validateStatus.isOK()) {
            return validateStatus;
        }

        buildPlannerInfo(shardResults, mongosStageName, out);
        buildExecStats(shardResults, mongosStageName, millisElapsed, out);

        return Status::OK();
    }

} // namespace mongo
