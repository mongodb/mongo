// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/util/modules.h"

#include <memory>
#include <type_traits>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class DeleteRequest;

/**
 * Get the YieldPolicy, adjusted for GodMode.
 */
PlanYieldPolicy::YieldPolicy getDeleteYieldPolicy(const DeleteRequest* request);

/**
 * ParsedDelete is a struct that holds the parsed query from a DeleteRequest. It is produced by
 * parsed_delete_command::parse().
 *
 * The filter is parsed into a ParsedFindCommand unless it is a simple _id equality (idhack
 * fast path). The ParsedFindCommand is not yet optimized and has not undergone timeseries
 * transformations.
 *
 * ParsedDelete is later consumed by CanonicalDelete::make(), which constructs a CanonicalDelete and
 * does timeseries transformations.
 */
struct [[MONGO_MOD_PUBLIC]] ParsedDelete {
    /**
     * Get the raw request.
     */
    const DeleteRequest* getRequest() const;

    /**
     * Returns true when the filter was parsed into a ParsedFindCommand. Returns false for the
     * idhack fast path.
     */
    bool hasParsedFindCommand() const;

    PlanYieldPolicy::YieldPolicy yieldPolicy() const {
        return getDeleteYieldPolicy(request);
    }

    // Unowned pointer to the request object to process. The pointer must outlive this object.
    const DeleteRequest* request;

    // Parsed query object, or NULL if the query proves to be an id hack query.
    std::unique_ptr<ParsedFindCommand> parsedFind;

    // Reference to an extensions callback used when parsing to a canonical query.
    std::unique_ptr<const ExtensionsCallback> extensionsCallback;
};

namespace parsed_delete_command {

/**
 * Parses the delete request and returns ParsedDelete.
 *
 * Note: As ExtensionsCallbackReal is available only on the mongod, mongos will pass an
 * ExtensionsCallbackNoop for parameter 'extensionsCallback' while mongod would use
 * ExtensionsCallbackReal.
 */
StatusWith<ParsedDelete> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                               const DeleteRequest* request,
                               std::unique_ptr<const ExtensionsCallback> extensionsCallback);

}  // namespace parsed_delete_command

}  // namespace mongo
