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

namespace MONGO_MOD_PUBLIC mongo {

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
struct MONGO_MOD_PUBLIC ParsedDelete {
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

}  // namespace MONGO_MOD_PUBLIC mongo
