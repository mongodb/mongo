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
#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
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

namespace impl {

/**
 * Note: this class is the base class for ParsedUpdate and ParsedUpdateForMongos. Their only
 * difference is that ParsedUpdateForMongos uses the ExtensionsCallbackNoop and on the other hand,
 * ParsedUpdate uses ExtensionsCallbackReal. The reason for this is that ExtensionsCallbackReal is
 * available only on the mongod. This difference does not need to be exposed through the interface
 * and can be hidden in the implementation.
 *
 */
class ParsedUpdateBase {
    ParsedUpdateBase(const ParsedUpdateBase&) = delete;
    ParsedUpdateBase& operator=(const ParsedUpdateBase&) = delete;

public:
    /**
     * Constructs a parsed update.
     *
     * The objects pointed to by "request" must stay in scope for the life of the constructed
     * ParsedUpdate.
     */
    ParsedUpdateBase(OperationContext* opCtx,
                     const UpdateRequest* request,
                     std::unique_ptr<const ExtensionsCallback> extensionsCallback,
                     const CollectionPtr& collection,
                     bool forgoOpCounterIncrements = false,
                     bool isRequestToTimeseries = false);

    /**
     * Parses the update request to a canonical query and an update driver. On success, the
     * parsed update can be used to create a PlanExecutor for this update.
     */
    Status parseRequest();

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
        invariant(_canonicalQuery);
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
    /**
     * Parses the query portion of the update request.
     */
    Status parseQuery();

    /**
     * Parses the update-descriptor portion of the update request.
     */
    void parseUpdate();

    /**
     * Handles splitting and/or translating the timeseries query predicate, if applicable. Must be
     * called before parsing the query and update.
     */
    void maybeTranslateTimeseriesUpdate();

    // Unowned pointer to the transactional context.
    OperationContext* _opCtx;

    // Unowned pointer to the request object to process.
    const UpdateRequest* const _request;

    // The array filters for the parsed update. Owned here.
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> _arrayFilters;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // Driver for processing updates on matched documents.
    UpdateDriver _driver;

    // Requested update modifications on matched documents.
    std::unique_ptr<write_ops::UpdateModification> _modification;

    // Parsed query object, or NULL if the query proves to be an id hack query.
    std::unique_ptr<CanonicalQuery> _canonicalQuery;

    // Reference to an extensions callback used when parsing to a canonical query.
    std::unique_ptr<const ExtensionsCallback> _extensionsCallback;

    // Reference to the collection this update is being performed on.
    const CollectionPtr& _collection;

    // Contains the residual expression and the bucket-level expression that should be pushed down
    // to the bucket collection.
    std::unique_ptr<TimeseriesWritesQueryExprs> _timeseriesUpdateQueryExprs;

    // The original, complete and untranslated write query expression.
    std::unique_ptr<MatchExpression> _originalExpr = nullptr;

    const bool _isRequestToTimeseries;
};

template <typename T, typename... Ts>
requires std::is_same_v<T, ExtensionsCallbackNoop> || std::is_same_v<T, ExtensionsCallbackReal>
std::unique_ptr<ExtensionsCallback> makeExtensionsCallback(Ts&&... args) {
    return std::make_unique<T>(std::forward<Ts>(args)...);
}

}  // namespace impl

/**
 * This class takes a pointer to an UpdateRequest, and converts that request into a parsed form
 * via the parseRequest() method. A ParsedUpdate can then be used to get information about the
 * update, or to retrieve an upsert document.
 *
 * No locks need to be held during parsing.
 *
 * The query part of the update is parsed to a CanonicalQuery, and the update part is parsed
 * using the UpdateDriver.
 *
 * ParsedUpdateForMongos is a ParsedUpdate that can be used in mongos.
 */
class ParsedUpdateForMongos : public impl::ParsedUpdateBase {
public:
    ParsedUpdateForMongos(OperationContext* opCtx, const UpdateRequest* request)
        : ParsedUpdateBase(opCtx,
                           request,
                           impl::makeExtensionsCallback<ExtensionsCallbackNoop>(),
                           CollectionPtr::null) {}
};

/**
 * This class takes a pointer to an UpdateRequest, and converts that request into a parsed form
 * via the parseRequest() method. A ParsedUpdate can then be used to retrieve a PlanExecutor
 * capable of executing the update.
 *
 * It is invalid to request that the UpdateStage return the prior or newly-updated version of a
 * document during a multi-update. It is also invalid to request that a ProjectionStage be
 * applied to the UpdateStage if the UpdateStage would not return any document.
 *
 * The query part of the update is parsed to a CanonicalQuery, and the update part is parsed
 * using the UpdateDriver.
 *
 * ParsedUpdate is a ParsedUpdate that can be used in mongod.
 */
class ParsedUpdate : public impl::ParsedUpdateBase {
public:
    ParsedUpdate(OperationContext* opCtx,
                 const UpdateRequest* request,
                 const CollectionPtr& collection,
                 bool forgoOpCounterIncrements = false,
                 bool isRequestToTimeseries = false);
};

}  // namespace mongo
