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

#include "mongo/db/query/write_ops/canonical_update.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
/**
 * Adds closed bucket filtering to query for timeseries multi-updates
 */
std::unique_ptr<MatchExpression> getClosedBucketFilteredExpr(
    boost::intrusive_ptr<ExpressionContext> expCtx, const UpdateRequest* request) {
    MatchExpressionParser::AllowedFeatureSet allowedFeatures =
        MatchExpressionParser::kAllowAllSpecialFeatures;
    if (request->isUpsert()) {
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
    }
    auto buildingExpr = uassertStatusOK(MatchExpressionParser::parse(
        request->getQuery(), expCtx, ExtensionsCallbackNoop(), allowedFeatures));
    buildingExpr = normalizeMatchExpression(std::move(buildingExpr));
    return timeseries::addClosedBucketExclusionExpr(std::move(buildingExpr));
}

struct TranslatedTimeseriesUpdate {
    // Contains the residual expression and the bucket-level expression that should be pushed down
    // to the bucket collection.
    std::unique_ptr<TimeseriesWritesQueryExprs> timeseriesUpdateQueryExprs;

    // The original, complete and untranslated write query expression.
    std::unique_ptr<MatchExpression> originalExpr = nullptr;
};

/**
 * Handles splitting and/or translating the timeseries query predicate, if applicable. Must be
 * called before parsing the query and update.
 */
TranslatedTimeseriesUpdate maybeTranslateTimeseriesUpdate(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<TimeseriesWritesQueryExprs> timeseriesUpdateQueryExprs,
    const UpdateRequest& request,
    const CollectionPtr& collection) {
    if (!timeseriesUpdateQueryExprs) {
        return {};
    }

    TranslatedTimeseriesUpdate out;
    out.timeseriesUpdateQueryExprs = std::move(timeseriesUpdateQueryExprs);

    // TODO: Due to the complexity which is related to the efficient sort support, we don't support
    // yet findAndModify with a query and sort but it should not be impossible. This code assumes
    // that in findAndModify code path, the parsed update constructor should be called with
    // isRequestToTimeseries = true for a time-series collection.
    uassert(ErrorCodes::InvalidOptions,
            "Cannot perform an updateOne or a findAndModify with a query and sort on a time-series "
            "collection.",
            request.isMulti() || request.getSort().isEmpty());

    // If we're updating documents in a time-series collection, splits the match expression into a
    // bucket-level match expression and a residual expression so that we can push down the
    // bucket-level match expression to the system bucket collection scan or fetch/ixscan.
    *out.timeseriesUpdateQueryExprs =
        timeseries::getMatchExprsForWrites(expCtx,
                                           *collection->getTimeseriesOptions(),
                                           request.getQuery(),
                                           collection->areTimeseriesBucketsFixed());

    // At this point, we parsed user-provided match expression. After this point, the new canonical
    // query is internal to the bucket SCAN or FETCH and will have additional internal match
    // expression. We do not need to track the internal match expression counters and so we stop the
    // counters because we do not want to count the internal match expression.
    expCtx->stopExpressionCounters();

    // We also need a copy of the original match expression to use for upserts and positional
    // updates.
    MatchExpressionParser::AllowedFeatureSet allowedFeatures =
        MatchExpressionParser::kAllowAllSpecialFeatures;
    if (request.isUpsert()) {
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
    }
    out.originalExpr = uassertStatusOK(MatchExpressionParser::parse(
        request.getQuery(), expCtx, ExtensionsCallbackNoop(), allowedFeatures));
    out.originalExpr = normalizeMatchExpression(std::move(out.originalExpr));
    return out;
}
}  // namespace

Status CanonicalUpdate::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    // _timeseriesUpdateQueryExprs may be null even if _isRequestToTimeseries is true. See
    // createTimeseriesWritesQueryExprsIfNecessary() for details.
    auto statusWithCQ = impl::parseWriteQueryToCQ(
        _expCtx.get(),
        *_extensionsCallback,
        *_request,
        _timeseriesUpdateQueryExprs  ? _timeseriesUpdateQueryExprs->_bucketExpr.get()
            : _isRequestToTimeseries ? getClosedBucketFilteredExpr(_expCtx, _request).get()
                                     : nullptr);

    if (statusWithCQ.isOK()) {
        _canonicalQuery = std::move(statusWithCQ.getValue());
    }

    if (statusWithCQ.getStatus().code() == ErrorCodes::QueryFeatureNotAllowed) {
        // The default error message for disallowed $expr is not descriptive enough, so we rewrite
        // it here.
        return {ErrorCodes::QueryFeatureNotAllowed,
                "$expr is not allowed in the query predicate for an upsert"};
    }

    return statusWithCQ.getStatus();
}

PlanYieldPolicy::YieldPolicy CanonicalUpdate::yieldPolicy() const {
    return getUpdateYieldPolicy(_request);
}

bool CanonicalUpdate::hasParsedQuery() const {
    return _canonicalQuery.get() != nullptr;
}

std::unique_ptr<CanonicalQuery> CanonicalUpdate::releaseParsedQuery() {
    tassert(11052007,
            "Expected CanonicalUpdate to own a CanonicalQuery",
            _canonicalQuery.get() != nullptr);
    return std::move(_canonicalQuery);
}

const UpdateRequest* CanonicalUpdate::getRequest() const {
    return _request;
}

UpdateDriver* CanonicalUpdate::getDriver() {
    return _driver.get();
}

bool CanonicalUpdate::isEligibleForArbitraryTimeseriesUpdate() const {
    return _timeseriesUpdateQueryExprs.get() != nullptr;
}

CanonicalUpdate::CanonicalUpdate(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    bool isRequestToTimeseries,
    std::unique_ptr<TimeseriesWritesQueryExprs> timeseriesUpdateQueryExprs,
    std::unique_ptr<MatchExpression> originalExpr,
    ParsedUpdate&& parsedUpdate,
    std::unique_ptr<CanonicalQuery>&& cq)
    : _request(std::move(parsedUpdate.request)),
      _arrayFilters(std::move(parsedUpdate.arrayFilters)),
      _expCtx(std::move(expCtx)),
      _driver(std::move(parsedUpdate.driver)),
      _modification(std::move(parsedUpdate.modification)),
      _canonicalQuery(std::move(cq)),
      _extensionsCallback(std::move(parsedUpdate.extensionsCallback)),
      _timeseriesUpdateQueryExprs(std::move(timeseriesUpdateQueryExprs)),
      _originalExpr(std::move(originalExpr)),
      _isRequestToTimeseries(isRequestToTimeseries) {}

StatusWith<std::unique_ptr<CanonicalUpdate>> CanonicalUpdate::make(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    ParsedUpdate&& parsedUpdate,
    const CollectionPtr& collection,
    bool isRequestToTimeseries) {

    // Start translating timeseries update.
    TranslatedTimeseriesUpdate translatedTimeseriesUpdate;
    if (isRequestToTimeseries) {
        auto timeseriesUpdateQueryExprs = createTimeseriesWritesQueryExprsIfNecessary(
            feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                VersionContext::getDecoration(expCtx->getOperationContext()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()),
            collection);

        translatedTimeseriesUpdate = maybeTranslateTimeseriesUpdate(
            expCtx, std::move(timeseriesUpdateQueryExprs), *parsedUpdate.request, collection);

        // Replace with the rewritten filter.
        if (parsedUpdate.hasParsedFindCommand()) {
            parsedUpdate.parsedFind->filter = translatedTimeseriesUpdate.timeseriesUpdateQueryExprs
                ? translatedTimeseriesUpdate.timeseriesUpdateQueryExprs->_bucketExpr->clone()
                : getClosedBucketFilteredExpr(expCtx, parsedUpdate.request);
            parsedUpdate.parsedFind->filter->setCollator(expCtx->getCollator());
            parsedUpdate.parsedFind->findCommandRequest->setFilter(
                parsedUpdate.parsedFind->filter->serialize());
        }
    }

    std::unique_ptr<CanonicalQuery> cq;
    if (parsedUpdate.hasParsedFindCommand()) {
        auto swCq = CanonicalQuery::make(
            {.expCtx = expCtx, .parsedFind = std::move(parsedUpdate.parsedFind)});
        if (!swCq.isOK()) {
            return swCq.getStatus();
        }
        cq = std::move(swCq.getValue());
    }
    return std::make_unique<CanonicalUpdate>(
        expCtx,
        isRequestToTimeseries,
        std::move(translatedTimeseriesUpdate.timeseriesUpdateQueryExprs),
        std::move(translatedTimeseriesUpdate.originalExpr),
        std::move(parsedUpdate),
        std::move(cq));
}

}  // namespace mongo
