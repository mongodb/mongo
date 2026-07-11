// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <vector>

namespace mongo {

template <typename T>
class StatusWith;
class CanonicalQuery;
class OperationContext;
struct ReadPreferenceSetting;

/**
 * Methods for running find and getMore operations across a sharded cluster.
 */
class [[MONGO_MOD_PUBLIC]] ClusterFind {
public:
    // The number of times we are willing to re-target and re-run the query after receiving a stale
    // config, snapshot, or shard not found error.
    static const size_t kMaxRetries;

    /**
     * Runs query 'query', targeting remote hosts according to the read preference in 'readPref'.
     *
     * On success, fills out 'results' with the first batch of query results and returns the cursor
     * id which the caller can use on subsequent getMore operations. If no cursor needed to be saved
     * (e.g. the cursor was exhausted without need for a getMore), returns a cursor id of 0.
     */
    static void runQuery(OperationContext* opCtx,
                         std::unique_ptr<FindCommandRequest> originalRequest,
                         const NamespaceString& origNss,
                         const ReadPreferenceSetting& readPref,
                         const MatchExpressionParser::AllowedFeatureSet& allowedFeatures,
                         rpc::ReplyBuilderInterface* result,
                         bool didDoFLERewrite = false);

    /**
     * Executes the getMore command 'cmd', and on success returns a CursorResponse.
     */
    static StatusWith<CursorResponse> runGetMore(OperationContext* opCtx,
                                                 const GetMoreCommandRequest& cmd);

    /**
     * Given the query being executed by mongos, returns a copy of the
     * query which is suitable for forwarding to the targeted hosts.
     */
    static StatusWith<std::unique_ptr<FindCommandRequest>> transformQueryForShards(
        const CanonicalQuery& query);

    /**
     * Generates a CanonicalQuery for the given request
     */
    static std::unique_ptr<CanonicalQuery> generateAndValidateCanonicalQuery(
        OperationContext* opCtx,
        const NamespaceString& origNss,
        std::unique_ptr<FindCommandRequest> cmdRequest,
        boost::optional<ExplainOptions::Verbosity> explain,
        const MatchExpressionParser::AllowedFeatureSet& allowedFeatures,
        bool mustRegisterRequestToQueryStats);
};

}  // namespace mongo
