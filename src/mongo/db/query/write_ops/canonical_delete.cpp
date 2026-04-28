/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/write_ops/canonical_delete.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
/**
 * Splits the user's query into a bucket-level match expression (pushed down to the system buckets
 * collection) and a residual expression (applied to the unpacked measurements), and propagates
 * the collection's extended-range-support flag onto 'expCtx'. Callers should gate this on
 * isRequestToTimeseries.
 *
 * Returns nullptr when the timeseries-deletes feature is disabled or the collection is not a
 * timeseries collection.
 */
std::unique_ptr<TimeseriesWritesQueryExprs> maybeTranslateTimeseriesDelete(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const DeleteRequest& request,
    const CollectionPtr& collection) {
    auto* opCtx = expCtx->getOperationContext();
    auto timeseriesDeleteQueryExprs = createTimeseriesWritesQueryExprsIfNecessary(
        feature_flags::gTimeseriesDeletesSupport.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()),
        collection);

    if (collection && collection->getRequiresTimeseriesExtendedRangeSupport()) {
        expCtx->setRequiresTimeseriesExtendedRangeSupport(true);
    }

    if (!timeseriesDeleteQueryExprs) {
        return nullptr;
    }

    // TODO: Due to the complexity which is related to the efficient sort support, we don't
    // support yet findAndModify with a query and sort but it should not be impossible. This
    // code assumes that in findAndModify code path, make() is called with
    // isRequestToTimeseries = true for a time-series collection.
    uassert(ErrorCodes::InvalidOptions,
            "Cannot perform a findAndModify with a query and sort on a time-series collection.",
            request.getMulti() || request.getSort().isEmpty());

    // If we're deleting documents from a time-series collection, splits the match expression
    // into a bucket-level match expression and a residual expression so that we can push down
    // the bucket-level match expression to the system bucket collection SCAN or FETCH/IXSCAN.
    *timeseriesDeleteQueryExprs =
        timeseries::getMatchExprsForWrites(expCtx,
                                           *collection->getTimeseriesOptions(),
                                           request.getQuery(),
                                           collection->areTimeseriesBucketsFixed());

    if (request.getMulti() && !timeseriesDeleteQueryExprs->_residualExpr) {
        timeseriesCounters.incrementMetaDelete();
    }

    tassert(
        7542400, "Bucket-level filter must not be null", timeseriesDeleteQueryExprs->_bucketExpr);
    return timeseriesDeleteQueryExprs;
}
}  // namespace

CanonicalDelete::CanonicalDelete(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    bool isRequestToTimeseries,
    std::unique_ptr<TimeseriesWritesQueryExprs> timeseriesDeleteQueryExprs,
    ParsedDelete&& parsedDelete,
    std::unique_ptr<CanonicalQuery>&& cq)
    : _request(parsedDelete.request),
      _expCtx(std::move(expCtx)),
      _canonicalQuery(std::move(cq)),
      _extensionsCallback(std::move(parsedDelete.extensionsCallback)),
      _timeseriesDeleteQueryExprs(std::move(timeseriesDeleteQueryExprs)),
      _isRequestToTimeseries(isRequestToTimeseries) {}

StatusWith<CanonicalDelete> CanonicalDelete::make(boost::intrusive_ptr<ExpressionContext> expCtx,
                                                  ParsedDelete&& parsed,
                                                  const CollectionPtr& collection,
                                                  bool isRequestToTimeseries) {

    const DeleteRequest* request = parsed.request;
    std::unique_ptr<TimeseriesWritesQueryExprs> timeseriesDeleteQueryExprs;

    if (isRequestToTimeseries) {
        timeseriesDeleteQueryExprs = maybeTranslateTimeseriesDelete(expCtx, *request, collection);

        // Swap the raw user filter with translated timeseries filter.
        if (parsed.hasParsedFindCommand() && timeseriesDeleteQueryExprs) {
            parsed.parsedFind->filter = timeseriesDeleteQueryExprs->_bucketExpr->clone();
            parsed.parsedFind->filter->setCollator(expCtx->getCollator());
            parsed.parsedFind->findCommandRequest->setFilter(
                parsed.parsedFind->filter->serialize());
        }
    }

    std::unique_ptr<CanonicalQuery> cq;
    // idHack path skips this and we fall back to parseQueryToCQ() in get_executor.cpp for
    // timeseries idhack queries.
    if (parsed.hasParsedFindCommand()) {
        auto swCq =
            CanonicalQuery::make({.expCtx = expCtx, .parsedFind = std::move(parsed.parsedFind)});
        if (!swCq.isOK()) {
            return swCq.getStatus();
        }
        cq = std::move(swCq.getValue());
    }

    return CanonicalDelete{std::move(expCtx),
                           isRequestToTimeseries,
                           std::move(timeseriesDeleteQueryExprs),
                           std::move(parsed),
                           std::move(cq)};
}

StatusWith<CanonicalDelete> CanonicalDelete::makeFromRequest(OperationContext* opCtx,
                                                             const CollectionPtr& collection,
                                                             const DeleteRequest& request,
                                                             bool isRequestToTimeseries) {
    auto [collatorToUse, collationMatchesDefault] =
        resolveCollator(opCtx, request.getCollation(), collection);
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, request)
                      .collator(std::move(collatorToUse))
                      .collationMatchesDefault(collationMatchesDefault)
                      .build();

    auto swParsedDelete = parsed_delete_command::parse(
        expCtx,
        &request,
        makeExtensionsCallback<ExtensionsCallbackReal>(opCtx, &request.getNsString()));
    if (!swParsedDelete.isOK()) {
        return swParsedDelete.getStatus();
    }
    return make(
        std::move(expCtx), std::move(swParsedDelete.getValue()), collection, isRequestToTimeseries);
}

Status CanonicalDelete::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    auto statusWithCQ = mongo::impl::parseWriteQueryToCQ(
        _expCtx.get(),
        *_extensionsCallback,
        *_request,
        _timeseriesDeleteQueryExprs ? _timeseriesDeleteQueryExprs->_bucketExpr.get() : nullptr);

    if (statusWithCQ.isOK()) {
        _canonicalQuery = std::move(statusWithCQ.getValue());
    }

    return statusWithCQ.getStatus();
}

const DeleteRequest* CanonicalDelete::getRequest() const {
    return _request;
}

PlanYieldPolicy::YieldPolicy CanonicalDelete::yieldPolicy() const {
    return getDeleteYieldPolicy(_request);
}

bool CanonicalDelete::hasParsedQuery() const {
    return _canonicalQuery.get() != nullptr;
}

std::unique_ptr<CanonicalQuery> CanonicalDelete::releaseParsedQuery() {
    tassert(11052003,
            "Expected CanonicalDelete to own a CanonicalQuery",
            _canonicalQuery.get() != nullptr);
    return std::move(_canonicalQuery);
}

bool CanonicalDelete::isEligibleForArbitraryTimeseriesDelete() const {
    return _timeseriesDeleteQueryExprs && (getResidualExpr() || !_request->getMulti());
}

}  // namespace mongo
