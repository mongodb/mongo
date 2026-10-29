// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class UpdateRequest;

/**
 * Get the YieldPolicy, adjusted for GodMode.
 */
PlanYieldPolicy::YieldPolicy getUpdateYieldPolicy(const UpdateRequest* request);


/**
 * ParsedUpdate is a struct that holds the parsed query and update from an UpdateRequest. It is
 * produced by parsed_update_command::parse().
 *
 * - The query part is parsed into a ParsedFindCommand, which at this stage is not yet optimized
 *   and has not undergone timeseries transformation.
 * - The update part is parsed into an UpdateDriver.
 *
 * ParsedUpdate is later consumed by CanonicalUpdate::make(), which constructs a CanonicalUpdate.
 */
struct [[MONGO_MOD_PUBLIC]] ParsedUpdate {
    /**
     * Get the raw request.
     */
    const UpdateRequest* getRequest() const;

    /**
     * Get a pointer to the update driver, the abstraction which both parses the update and
     * is capable of applying mods / computing damages.
     */
    UpdateDriver* getDriver();

    /**
     * Get a read-only pointer to the update driver.
     */
    const UpdateDriver* getDriver() const;

    /**
     * As an optimization, we don't create a parsed find command for updates with simple _id
     * queries. Use this method to determine whether or not we actually parsed the query.
     */
    bool hasParsedFindCommand() const;

    inline PlanYieldPolicy::YieldPolicy yieldPolicy() const {
        return getUpdateYieldPolicy(request);
    }

    // Unowned pointer to the request object to process. The pointer must outlive this object.
    const UpdateRequest* request;

    // The array filters for the parsed update. Owned here.
    std::unique_ptr<std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>>>
        arrayFilters;

    // Driver for processing updates on matched documents.
    std::unique_ptr<UpdateDriver> driver;

    // Requested update modifications on matched documents.
    std::unique_ptr<write_ops::UpdateModification> modification;

    // Parsed query object, or NULL if the query proves to be an id hack query.
    std::unique_ptr<ParsedFindCommand> parsedFind;

    // Reference to an extensions callback used when parsing to a canonical query.
    std::unique_ptr<const ExtensionsCallback> extensionsCallback;
};

namespace parsed_update_command {

/**
 * Parses the update request and returns ParsedUpdate.
 *
 * Note: As ExtensionsCallbackReal is available only on the mongod, mongos will pass an
 * ExtensionsCallbackNoop for parameter 'extensionsCallback' while mongod would use
 * ExtensionsCallbackReal.
 */
StatusWith<ParsedUpdate> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                               const UpdateRequest* request,
                               std::unique_ptr<const ExtensionsCallback> extensionsCallback);

}  // namespace parsed_update_command

}  // namespace mongo
