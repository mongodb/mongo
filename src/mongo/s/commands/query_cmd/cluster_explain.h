// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <vector>

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
     * level, pruning any generic arguments in the inner command as they should already be provided
     * on the top-level outer commmand. Additionally, the passed command is appended with query
     * settings.
     */
    static BSONObj wrapAsExplain(const BSONObj& cmdObj,
                                 ExplainOptions::Verbosity verbosity,
                                 const BSONObj& querySettings = BSONObj());

    /**
     * Determines the kind of "execution stage" that mongos would use in order to collect the
     * responses from the shards, assuming that the command being explained is a read operation
     * such as find or count.
     *
     * Accepts the parsed command request object (e.g. FindCommandRequest, CountCommandRequest).
     */
    template <typename RequestType>
    static const char* getStageNameForReadOp(size_t numShards, const RequestType& request) {
        if (numShards == 1) {
            return kSingleShard;
        } else if constexpr (requires { request.getSort(); }) {
            if (!request.getSort().isEmpty()) {
                return kMergeSortFromShards;
            }
        }
        return kMergeFromShards;
    }

    /**
     * When a database is not found, use this method to construct an EOF plan explain() response.
     */
    static void buildEOFExplainResult(OperationContext* opCtx,
                                      const CanonicalQuery* cq,
                                      const BSONObj& command,
                                      BSONObjBuilder* out);

    /**
     * Command implementations on mongos use this method to construct the sharded explain
     * output format based on the responses from the shards in 'shardResponses'.
     *
     * On success, the output is added to the BSONObj builder 'out'.
     */
    static Status buildExplainResult(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::vector<AsyncRequestsSender::Response>& shardResponses,
        const char* mongosStageName,
        long long millisElapsed,
        const BSONObj& command,
        BSONObjBuilder* out,
        boost::optional<int64_t> limit = boost::none,
        boost::optional<int64_t> skip = boost::none);


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
                               BSONObjBuilder* out,
                               boost::optional<int64_t> limit,
                               boost::optional<int64_t> skip);
};

}  // namespace mongo
