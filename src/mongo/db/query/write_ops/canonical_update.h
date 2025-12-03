/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"

#include <map>
#include <memory>
#include <type_traits>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class CanonicalQuery;
class ExtensionsCallbackNoop;
class ExtensionsCallbackReal;
class OperationContext;
class UpdateRequest;

/**
 * This class is constructed from ParsedUpdate with CanonicalUpdate::make() method. A
 * CanonicalUpdate can then be used to retrieve a PlanExecutor capable of executing the update.
 *
 * It is invalid to request that the UpdateStage return the prior or newly-updated version of a
 * document during a multi-update. It is also invalid to request that a ProjectionStage be
 * applied to the UpdateStage if the UpdateStage would not return any document.
 *
 * The query part of the update is parsed to a CanonicalQuery, which is originally a
 * ParsedFindCommand.
 */
class CanonicalUpdate {
    CanonicalUpdate(const CanonicalUpdate&) = delete;
    CanonicalUpdate& operator=(const CanonicalUpdate&) = delete;

public:
    CanonicalUpdate(boost::intrusive_ptr<ExpressionContext> expCtx,
                    bool isRequestToTimeseries,
                    std::unique_ptr<TimeseriesWritesQueryExprs> timeseriesUpdateQueryExprs,
                    std::unique_ptr<MatchExpression> originalExpr,
                    ParsedUpdate&& parsedUpdate,
                    std::unique_ptr<CanonicalQuery>&& cq);

    static StatusWith<std::unique_ptr<CanonicalUpdate>> make(
        boost::intrusive_ptr<ExpressionContext> expCtx,
        ParsedUpdate&& parsedUpdate,
        const CollectionPtr& collection = CollectionPtr::null,
        bool isRequestToTimeseries = false);

    /**
     * As an optimization, we do not create a canonical query if the predicate is a simple
     * _id equality. This method can be used to force full parsing to a canonical query,
     * as a fallback if the idhack path is not available (e.g. no _id index).
     */
    Status parseQueryToCQ();

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
     * Get the YieldPolicy, adjusted for GodMode.
     */
    PlanYieldPolicy::YieldPolicy yieldPolicy() const;

    /**
     * As an optimization, we don't create a canonical query for updates with simple _id
     * queries. Use this method to determine whether or not we actually parsed the query.
     */
    bool hasParsedQuery() const;

    /**
     * Returns a const pointer to the canonical query. Requires that hasParsedQuery() is true.
     */
    const CanonicalQuery* getParsedQuery() const {
        tassert(11052008, "Expected CanonicalQuery to exist", _canonicalQuery);
        return _canonicalQuery.get();
    }

    /**
     * Releases ownership of the canonical query to the caller.
     */
    std::unique_ptr<CanonicalQuery> releaseParsedQuery();

    /**
     * Never returns nullptr.
     */
    boost::intrusive_ptr<ExpressionContext> expCtx() const {
        return _expCtx;
    }

    /**
     * Releases the ownership of the residual MatchExpression.
     *
     * Note: see _timeseriesUpdateQueryExprs._bucketMatchExpr for more details.
     */
    std::unique_ptr<MatchExpression> releaseResidualExpr() {
        return _timeseriesUpdateQueryExprs ? std::move(_timeseriesUpdateQueryExprs->_residualExpr)
                                           : nullptr;
    }

    /**
     * Releases the ownership of the original MatchExpression.
     */
    std::unique_ptr<MatchExpression> releaseOriginalExpr() {
        return std::move(_originalExpr);
    }

    /**
     * Returns true when we are performing multi updates using a residual predicate on a time-series
     * collection or when performing singleton updates on a time-series collection.
     */
    bool isEligibleForArbitraryTimeseriesUpdate() const;

    bool isRequestToTimeseries() const {
        return _isRequestToTimeseries;
    }

private:
    // Unowned pointer to the request object to process.
    const UpdateRequest* const _request;

    // The array filters for the parsed update. Owned here.
    std::unique_ptr<std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>> _arrayFilters;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // Driver for processing updates on matched documents.
    std::unique_ptr<UpdateDriver> _driver;

    // Requested update modifications on matched documents.
    std::unique_ptr<write_ops::UpdateModification> _modification;

    // Parsed query object, or NULL if the query proves to be an id hack query.
    std::unique_ptr<CanonicalQuery> _canonicalQuery;

    // Reference to an extensions callback used when parsing to a canonical query.
    std::unique_ptr<const ExtensionsCallback> _extensionsCallback;

    // Contains the residual expression and the bucket-level expression that should be pushed down
    // to the bucket collection.
    std::unique_ptr<TimeseriesWritesQueryExprs> _timeseriesUpdateQueryExprs;

    // The original, complete and untranslated write query expression.
    std::unique_ptr<MatchExpression> _originalExpr = nullptr;

    const bool _isRequestToTimeseries;
};
}  // namespace mongo
