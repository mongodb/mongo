// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DeleteRequest;

/**
 * This class is constructed from a ParsedDelete with the CanonicalDelete::make() method. A
 * CanonicalDelete can then be used to retrieve a PlanExecutor capable of executing the delete.
 *
 * It is invalid to request that the DeleteStage return the deleted document during a
 * multi-remove. It is also invalid to request that a ProjectionStage be applied to the
 * DeleteStage if the DeleteStage would not return the deleted document.
 *
 * The query part of the delete is parsed to a CanonicalQuery, which is originally a
 * ParsedFindCommand.
 */
class [[MONGO_MOD_PUBLIC]] CanonicalDelete {
    CanonicalDelete(const CanonicalDelete&) = delete;
    CanonicalDelete& operator=(const CanonicalDelete&) = delete;

public:
    CanonicalDelete(CanonicalDelete&&) noexcept = default;

    CanonicalDelete(boost::intrusive_ptr<ExpressionContext> expCtx,
                    bool isRequestToTimeseries,
                    std::unique_ptr<TimeseriesWritesQueryExprs> timeseriesDeleteQueryExprs,
                    ParsedDelete&& parsedDelete,
                    std::unique_ptr<CanonicalQuery>&& cq);

    static StatusWith<CanonicalDelete> make(boost::intrusive_ptr<ExpressionContext> expCtx,
                                            ParsedDelete&& parsed,
                                            const CollectionPtr& collection,
                                            bool isRequestToTimeseries = false);

    /**
     * Convenience factory that builds the ExpressionContext, parses the DeleteRequest, and
     * invokes make(). Allows callers to go directly from a DeleteRequest to a CanonicalDelete.
     */
    static StatusWith<CanonicalDelete> makeFromRequest(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const DeleteRequest& request,
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
    const DeleteRequest* getRequest() const;

    /**
     * Get the YieldPolicy, adjusted for GodMode.
     */
    PlanYieldPolicy::YieldPolicy yieldPolicy() const;

    /**
     * As an optimization, we don't create a canonical query for deletes with simple _id
     * queries. Use this method to determine whether or not we actually parsed the query.
     */
    bool hasParsedQuery() const;

    /**
     * Releases ownership of the canonical query to the caller.
     */
    std::unique_ptr<CanonicalQuery> releaseParsedQuery();

    /**
     * Never returns nullptr.
     */
    boost::intrusive_ptr<ExpressionContext> expCtx() const {
        tassert(11052004, "Expected ExpressionContext to exist", _expCtx.get());
        return _expCtx;
    }

    /**
     * Returns the non-modifiable residual MatchExpression.
     *
     * Note: see TimeseriesWritesQueryExprs::_residualExpr for more details.
     */
    const MatchExpression* getResidualExpr() const {
        return _timeseriesDeleteQueryExprs ? _timeseriesDeleteQueryExprs->_residualExpr.get()
                                           : nullptr;
    }

    /**
     * Releases the ownership of the residual MatchExpression.
     *
     * Note: see TimeseriesWritesQueryExprs::_bucketExpr for more details.
     */
    std::unique_ptr<MatchExpression> releaseResidualExpr() {
        return _timeseriesDeleteQueryExprs ? std::move(_timeseriesDeleteQueryExprs->_residualExpr)
                                           : nullptr;
    }

    /**
     * Returns true when we are performing multi deletes using a residual predicate on a
     * time-series collection or when performing singleton deletes on a time-series collection.
     */
    bool isEligibleForArbitraryTimeseriesDelete() const;

    bool isRequestToTimeseries() const {
        return _isRequestToTimeseries;
    }

private:
    // Unowned pointer to the request object to process.
    const DeleteRequest* const _request;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // Parsed query object, or NULL if the query proves to be an id hack query.
    std::unique_ptr<CanonicalQuery> _canonicalQuery;

    // Reference to an extensions callback used when parsing to a canonical query.
    std::unique_ptr<const ExtensionsCallback> _extensionsCallback;

    // Contains the bucket-level expression and the residual expression. The bucket-level
    // expression should be pushed down to the bucket collection.
    std::unique_ptr<TimeseriesWritesQueryExprs> _timeseriesDeleteQueryExprs;

    const bool _isRequestToTimeseries;
};

}  // namespace mongo
