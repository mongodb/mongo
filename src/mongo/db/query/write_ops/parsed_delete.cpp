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

#include "mongo/db/query/write_ops/parsed_delete.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

// Note: The caller should hold a lock on the 'collection' if it really exists so that it can stay
// alive until the end of the ParsedDelete's lifetime.
ParsedDelete::ParsedDelete(OperationContext* opCtx,
                           const DeleteRequest* request,
                           const CollectionPtr& collection,
                           bool isTimeseriesDelete)
    : _opCtx(opCtx),
      _request(request),
      _collection(collection),
      _timeseriesDeleteQueryExprs(
          isTimeseriesDelete
              ? createTimeseriesWritesQueryExprsIfNecessary(
                    feature_flags::gTimeseriesDeletesSupport.isEnabled(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()),
                    collection)
              : nullptr),
      _isRequestToTimeseries(isTimeseriesDelete) {}

Status ParsedDelete::parseRequest() {
    dassert(!_canonicalQuery.get());
    // It is invalid to request that the DeleteStage return the deleted document during a
    // multi-remove.
    invariant(!(_request->getReturnDeleted() && _request->getMulti()));

    // It is invalid to request that a ProjectionStage be applied to the DeleteStage if the
    // DeleteStage would not return the deleted document.
    invariant(_request->getProj().isEmpty() || _request->getReturnDeleted());

    auto [collatorToUse, collationMatchesDefault] =
        resolveCollator(_opCtx, _request->getCollation(), _collection);
    _expCtx = ExpressionContextBuilder{}
                  .opCtx(_opCtx)
                  .collator(std::move(collatorToUse))
                  .ns(_request->getNsString())
                  .runtimeConstants(_request->getLegacyRuntimeConstants())
                  .letParameters(_request->getLet())
                  .collationMatchesDefault(collationMatchesDefault)
                  .build();

    // The '_id' field of a time-series collection needs to be handled as other fields.
    if (isSimpleIdQuery(_request->getQuery()) && !_timeseriesDeleteQueryExprs) {
        return Status::OK();
    }

    if (_isRequestToTimeseries && _collection &&
        _collection->getRequiresTimeseriesExtendedRangeSupport()) {
        _expCtx->setRequiresTimeseriesExtendedRangeSupport(true);
    }

    _expCtx->startExpressionCounters();

    if (auto&& queryExprs = _timeseriesDeleteQueryExprs) {
        // TODO: Due to the complexity which is related to the efficient sort support, we don't
        // support yet findAndModify with a query and sort but it should not be impossible. This
        // code assumes that in findAndModify code path, the parsed delete constructor should be
        // called with isTimeseriesDelete = true for a time-series collection.
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a findAndModify with a query and sort on a time-series collection.",
                _request->getMulti() || _request->getSort().isEmpty());

        // If we're deleting documents from a time-series collection, splits the match expression
        // into a bucket-level match expression and a residual expression so that we can push down
        // the bucket-level match expression to the system bucket collection SCAN or FETCH/IXSCAN.
        *_timeseriesDeleteQueryExprs =
            timeseries::getMatchExprsForWrites(_expCtx,
                                               *_collection->getTimeseriesOptions(),
                                               _request->getQuery(),
                                               _collection->areTimeseriesBucketsFixed());

        // At this point, we parsed user-provided match expression. After this point, the new
        // canonical query is internal to the bucket SCAN or FETCH/IXSCAN and will have additional
        // internal match expression. We do not need to track the internal match expression counters
        // and so we stop the counters.
        _expCtx->stopExpressionCounters();

        if (_request->getMulti() && !getResidualExpr()) {
            // The command is performing a time-series meta delete.
            timeseriesCounters.incrementMetaDelete();
        }

        // At least, the bucket-level filter must contain the closed bucket filter.
        tassert(7542400, "Bucket-level filter must not be null", queryExprs->_bucketExpr);
    }

    return parseQueryToCQ();
}

Status ParsedDelete::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    auto statusWithCQ = mongo::parseWriteQueryToCQ(
        _expCtx->getOperationContext(),
        _expCtx.get(),
        *_request,
        _timeseriesDeleteQueryExprs ? _timeseriesDeleteQueryExprs->_bucketExpr.get() : nullptr);

    if (statusWithCQ.isOK()) {
        _canonicalQuery = std::move(statusWithCQ.getValue());
    }

    return statusWithCQ.getStatus();
}

const DeleteRequest* ParsedDelete::getRequest() const {
    return _request;
}

PlanYieldPolicy::YieldPolicy ParsedDelete::yieldPolicy() const {
    return _request->getGod() ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                              : _request->getYieldPolicy();
}

bool ParsedDelete::hasParsedQuery() const {
    return _canonicalQuery.get() != nullptr;
}

std::unique_ptr<CanonicalQuery> ParsedDelete::releaseParsedQuery() {
    invariant(_canonicalQuery.get() != nullptr);
    return std::move(_canonicalQuery);
}

bool ParsedDelete::isEligibleForArbitraryTimeseriesDelete() const {
    return _timeseriesDeleteQueryExprs && (getResidualExpr() || !_request->getMulti());
}

}  // namespace mongo
