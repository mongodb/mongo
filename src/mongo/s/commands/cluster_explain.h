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

#pragma once

#include <string>

#include "mongo/db/query/explain_options.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/commands/strategy.h"

namespace mongo {

class OperationContext;

/**
 * Namespace for the collection of static methods used by commands in the implementation of explain
 * on mongos.
 */
class ClusterExplain {
public:
    /**
     * Returns an explain command request wrapping the passed in command at the given verbosity
     * level, propagating generic top-level command arguments.
     */
    static BSONObj wrapAsExplain(const BSONObj& cmdObj, ExplainOptions::Verbosity verbosity);

    /**
     * Determines the kind of "execution stage" that mongos would use in order to collect the
     * responses from the shards, assuming that the command being explained is a read operation
     * such as find or count.
     */
    static const char* getStageNameForReadOp(size_t numShards, const BSONObj& explainObj);

    /**
     * Command implementations on mongos use this method to construct the sharded explain
     * output format based on the responses from the shards in 'shardResponses'.
     *
     * On success, the output is added to the BSONObj builder 'out'.
     */
    static Status buildExplainResult(
        OperationContext* opCtx,
        const std::vector<AsyncRequestsSender::Response>& shardResponses,
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
     * responses. Otherwise, returns a non-OK status and the entire explain should fail.
     */
    static void validateShardResponses(
        const std::vector<AsyncRequestsSender::Response>& shardResponses);

    /**
     * Populates the BSONObj builder 'out' with query planner explain information, based on
     * the responses from the shards contained in 'shardResponses'.
     *
     * The planner info will display 'mongosStageName' as the name of the execution stage
     * performed by mongos after gathering responses from the shards.
     */
    static void buildPlannerInfo(OperationContext* opCtx,
                                 const std::vector<AsyncRequestsSender::Response>& shardResponses,
                                 const char* mongosStageName,
                                 BSONObjBuilder* out);

    /**
     * Populates the BSONObj builder 'out' with execution stats explain information,
     * if the responses from the shards in 'shardsResponses' contain this info.
     *
     * Will display 'mongosStageName' as the name of the execution stage performed by mongos,
     * and 'millisElapsed' as the execution time of the mongos stage.
     */
    static void buildExecStats(const std::vector<AsyncRequestsSender::Response>& shardResponses,
                               const char* mongosStageName,
                               long long millisElapsed,
                               BSONObjBuilder* out);
};

}  // namespace mongo
