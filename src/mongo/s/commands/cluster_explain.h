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

#pragma once

#include <string>

#include "mongo/db/query/explain_common.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/write_ops/batched_command_request.h"

namespace mongo {

class OperationContext;

namespace rpc {
class ServerSelectionMetadata;
}  // namespace rpc

/**
 * Namespace for the collection of static methods used by commands in the implementation of
 * explain on mongos.
 */
class ClusterExplain {
public:
    /**
     * Given the BSON specification for a command, 'cmdObj', wraps the object in order to
     * produce the BSON for an explain of that command, at the given verbosity level
     * 'verbosity' and according to the metadata in 'serverSelectionMetadata'.
     *
     * Adds the result to the BSONObj builder 'out'.
     *
     * Also uses 'serverSelectionMetdata' to set 'optionsOut' to the options bit vector that should
     * be forwarded to the shards.
     */
    static void wrapAsExplain(const BSONObj& cmdObj,
                              ExplainCommon::Verbosity verbosity,
                              const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                              BSONObjBuilder* out,
                              int* optionsOut);

    /**
     * Determines the kind of "execution stage" that mongos would use in order to collect
     * the results from the shards, assuming that the command being explained is a read
     * operation such as find or count.
     */
    static const char* getStageNameForReadOp(
        const std::vector<Strategy::CommandResult>& shardResults, const BSONObj& explainObj);

    /**
     * Command implementations on mongos use this method to construct the sharded explain
     * output format based on the results from the shards in 'shardResults'.
     *
     * On success, the output is added to the BSONObj builder 'out'.
     */
    static Status buildExplainResult(OperationContext* txn,
                                     const std::vector<Strategy::CommandResult>& shardResults,
                                     const char* mongosStageName,
                                     long long millisElapsed,
                                     BSONObjBuilder* out);

    //
    // Names of mock mongos execution stages.
    //

    static const char* kSingleShard;
    static const char* kMergeFromShards;
    static const char* kMergeSortFromShards;
    static const char* kWriteOnShards;

private:
    /**
     * Returns an OK status if all shards support the explain command and returned sensible
     * results. Otherwise, returns a non-OK status and the entire explain should fail.
     */
    static Status validateShardResults(const std::vector<Strategy::CommandResult>& shardResults);

    /**
     * Populates the BSONObj builder 'out' with query planner explain information, based on
     * the results from the shards contained in 'shardResults'.
     *
     * The planner info will display 'mongosStageName' as the name of the execution stage
     * performed by mongos after gathering results from the shards.
     */
    static void buildPlannerInfo(OperationContext* txn,
                                 const std::vector<Strategy::CommandResult>& shardResults,
                                 const char* mongosStageName,
                                 BSONObjBuilder* out);

    /**
     * Populates the BSONObj builder 'out' with execution stats explain information,
     * if the results from the shards in 'shardsResults' contain this info.
     *
     * Will display 'mongosStageName' as the name of the execution stage performed by mongos,
     * and 'millisElapsed' as the execution time of the mongos stage.
     */
    static void buildExecStats(const std::vector<Strategy::CommandResult>& shardResults,
                               const char* mongosStageName,
                               long long millisElapsed,
                               BSONObjBuilder* out);
};

}  // namespace mongo
