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

#include <memory>

#include "mongo/base/status.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/timeseries/timeseries_gen.h"

namespace mongo {

class CanonicalQuery;
class Database;
class DeleteRequest;
class OperationContext;

/**
 * This class takes a pointer to a DeleteRequest, and converts that request into a parsed form
 * via the parseRequest() method. A ParsedDelete can then be used to retrieve a PlanExecutor
 * capable of executing the delete.
 *
 * It is invalid to request that the DeleteStage return the deleted document during a
 * multi-remove. It is also invalid to request that a ProjectionStage be applied to the
 * DeleteStage if the DeleteStage would not return the deleted document.
 *
 * A delete request is parsed to a CanonicalQuery, so this class is a thin, delete-specific
 * wrapper around canonicalization.
 *
 * No locks need to be held during parsing.
 */
class ParsedDelete {
    ParsedDelete(const ParsedDelete&) = delete;
    ParsedDelete& operator=(const ParsedDelete&) = delete;

public:
    /**
     * Constructs a parsed delete for a regular delete which does not involve a time-series
     * collection.
     *
     * The object pointed to by "request" must stay in scope for the life of the constructed
     * ParsedDelete.
     */
    ParsedDelete(OperationContext* opCtx, const DeleteRequest* request)
        : ParsedDelete(opCtx, request, boost::none) {}

    /**
     * Constructs a parsed delete which may involve a time-series collection.
     *
     * The object pointed to by "request" must stay in scope for the life of the constructed
     * ParsedDelete.
     */
    ParsedDelete(OperationContext* opCtx,
                 const DeleteRequest* request,
                 boost::optional<TimeseriesOptions> timeseriesOptions);

    /**
     * Parses the delete request to a canonical query. On success, the parsed delete can be
     * used to create a PlanExecutor capable of executing this delete.
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
    const DeleteRequest* getRequest() const;

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
     * Releases ownership of the canonical query to the caller.
     */
    std::unique_ptr<CanonicalQuery> releaseParsedQuery();

    /**
     * This may return nullptr, specifically in cases where the query is IDHACK eligible.
     */
    const CanonicalQuery* parsedQuery() const {
        return _canonicalQuery.get();
    }

    /**
     * Always guaranteed to return a valid expression context.
     */
    boost::intrusive_ptr<ExpressionContext> expCtx() {
        invariant(_expCtx.get());
        return _expCtx;
    }

    void setCollator(std::unique_ptr<CollatorInterface> collator);

    /**
     * Returns the non-modifiable residual MatchExpression.
     *
     * Note: see _timeseriesDeleteDetails._residualExpr for more details.
     */
    const MatchExpression* getResidualExpr() const {
        return _timeseriesDeleteDetails ? _timeseriesDeleteDetails->_residualExpr.get() : nullptr;
    }

    /**
     * Releases the ownership of the residual MatchExpression.
     *
     * Note: see _timeseriesDeleteDetails._bucketMatchExpr for more details.
     */
    std::unique_ptr<MatchExpression> releaseResidualExpr() {
        return _timeseriesDeleteDetails ? std::move(_timeseriesDeleteDetails->_residualExpr)
                                        : nullptr;
    }

    /**
     * Returns true when we are performing multi deletes using a residual predicate on a time-series
     * collection or when performing singleton deletes on a time-series collection.
     */
    bool isEligibleForArbitraryTimeseriesDelete() const;

private:
    // Transactional context.  Not owned by us.
    OperationContext* _opCtx;

    // Unowned pointer to the request object that this executor will process.
    const DeleteRequest* const _request;

    // Parsed query object, or NULL if the query proves to be an id hack query.
    std::unique_ptr<CanonicalQuery> _canonicalQuery;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    Status splitOutBucketMatchExpression(const ExtensionsCallback& extensionsCallback);

    // Time-series deletes take some special handling to make sure we delete the buckets collection,
    // but interact with the documents as if they were unpacked.
    struct TimeseriesDeleteDetails {
        TimeseriesDeleteDetails(const TimeseriesOptions& timeseriesOptions)
            : _timeseriesOptions(timeseriesOptions) {}

        TimeseriesOptions _timeseriesOptions;

        // The bucket-level match expressions.
        std::unique_ptr<MatchExpression> _bucketExpr = nullptr;

        // The residual expression after splitting out metaField-dependent splittable match
        // expressions.
        std::unique_ptr<MatchExpression> _residualExpr = nullptr;
    };

    std::unique_ptr<TimeseriesDeleteDetails> _timeseriesDeleteDetails = nullptr;
};

}  // namespace mongo
