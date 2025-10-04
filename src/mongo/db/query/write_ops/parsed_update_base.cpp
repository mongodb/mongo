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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/parsed_update_array_filters.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/version_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::impl {

// Note: The caller should hold a lock on the 'collection' if it really exists so that it can stay
// alive until the end of the ParsedUpdate's lifetime.
ParsedUpdateBase::ParsedUpdateBase(OperationContext* opCtx,
                                   const UpdateRequest* request,
                                   std::unique_ptr<const ExtensionsCallback> extensionsCallback,
                                   const CollectionPtr& collection,
                                   bool forgoOpCounterIncrements,
                                   bool isRequestToTimeseries)
    : _opCtx(opCtx),
      _request(request),
      _expCtx(ExpressionContextBuilder{}
                  .opCtx(opCtx)
                  .ns(_request->getNamespaceString())
                  // mayDbProfile. We pass 'true' here conservatively. In the
                  // future we may change this.
                  .mayDbProfile(true)
                  .allowDiskUse(allowDiskUseByDefault.load())
                  .explain(_request->explain())
                  .runtimeConstants(_request->getLegacyRuntimeConstants())
                  .letParameters(_request->getLetParameters())
                  .isUpsert(request->isUpsert())
                  .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
                  .build()),
      _driver(_expCtx),
      _modification(
          std::make_unique<write_ops::UpdateModification>(_request->getUpdateModification())),
      _canonicalQuery(),
      _extensionsCallback(std::move(extensionsCallback)),
      _collection(collection),
      _timeseriesUpdateQueryExprs(
          isRequestToTimeseries
              ? createTimeseriesWritesQueryExprsIfNecessary(
                    feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()),
                    collection)
              : nullptr),
      _isRequestToTimeseries(isRequestToTimeseries) {
    if (forgoOpCounterIncrements) {
        _expCtx->setEnabledCounters(false);
    }
    tassert(
        7655104, "timeseries collection must already exist", _collection || !isRequestToTimeseries);

    if (isRequestToTimeseries && _collection &&
        _collection->getRequiresTimeseriesExtendedRangeSupport()) {
        _expCtx->setRequiresTimeseriesExtendedRangeSupport(true);
    }
}

std::unique_ptr<MatchExpression> ParsedUpdateBase::getClosedBucketFilteredExpr() {
    MatchExpressionParser::AllowedFeatureSet allowedFeatures =
        MatchExpressionParser::kAllowAllSpecialFeatures;
    if (_request->isUpsert()) {
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
    }
    auto buildingExpr = uassertStatusOK(MatchExpressionParser::parse(
        _request->getQuery(), _expCtx, ExtensionsCallbackNoop(), allowedFeatures));
    buildingExpr = normalizeMatchExpression(std::move(buildingExpr));
    return timeseries::addClosedBucketExclusionExpr(std::move(buildingExpr));
}

void ParsedUpdateBase::maybeTranslateTimeseriesUpdate() {
    if (!_timeseriesUpdateQueryExprs) {
        return;
    }

    // TODO: Due to the complexity which is related to the efficient sort support, we don't support
    // yet findAndModify with a query and sort but it should not be impossible. This code assumes
    // that in findAndModify code path, the parsed update constructor should be called with
    // isRequestToTimeseries = true for a time-series collection.
    uassert(ErrorCodes::InvalidOptions,
            "Cannot perform an updateOne or a findAndModify with a query and sort on a time-series "
            "collection.",
            _request->isMulti() || _request->getSort().isEmpty());

    // If we're updating documents in a time-series collection, splits the match expression into a
    // bucket-level match expression and a residual expression so that we can push down the
    // bucket-level match expression to the system bucket collection scan or fetch/ixscan.
    *_timeseriesUpdateQueryExprs =
        timeseries::getMatchExprsForWrites(_expCtx,
                                           *_collection->getTimeseriesOptions(),
                                           _request->getQuery(),
                                           _collection->areTimeseriesBucketsFixed());

    // At this point, we parsed user-provided match expression. After this point, the new canonical
    // query is internal to the bucket SCAN or FETCH and will have additional internal match
    // expression. We do not need to track the internal match expression counters and so we stop the
    // counters because we do not want to count the internal match expression.
    _expCtx->stopExpressionCounters();

    // We also need a copy of the original match expression to use for upserts and positional
    // updates.
    MatchExpressionParser::AllowedFeatureSet allowedFeatures =
        MatchExpressionParser::kAllowAllSpecialFeatures;
    if (_request->isUpsert()) {
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
    }
    _originalExpr = uassertStatusOK(MatchExpressionParser::parse(
        _request->getQuery(), _expCtx, ExtensionsCallbackNoop(), allowedFeatures));
    _originalExpr = normalizeMatchExpression(std::move(_originalExpr));
}

Status ParsedUpdateBase::parseRequest() {
    // It is invalid to request that the UpdateStage return the prior or newly-updated version
    // of a document during a multi-update.
    invariant(!(_request->shouldReturnAnyDocs() && _request->isMulti()));

    // It is invalid to specify 'upsertSupplied:true' for a non-upsert operation, or if no upsert
    // document was supplied with the request.
    if (_request->shouldUpsertSuppliedDocument()) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "cannot specify '"
                              << write_ops::UpdateOpEntry::kUpsertSuppliedFieldName
                              << ": true' for a non-upsert operation",
                _request->isUpsert());
        const auto& constants = _request->getUpdateConstants();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "the parameter '"
                              << write_ops::UpdateOpEntry::kUpsertSuppliedFieldName
                              << "' is set to 'true', but no document was supplied",
                constants && (*constants)["new"_sd].type() == BSONType::object);
    }

    // It is invalid to request that a ProjectionStage be applied to the UpdateStage if the
    // UpdateStage would not return any document.
    invariant(_request->getProj().isEmpty() || _request->shouldReturnAnyDocs());

    auto [collatorToUse, collationMatchesDefault] =
        resolveCollator(_opCtx, _request->getCollation(), _collection);
    _expCtx->setCollator(std::move(collatorToUse));
    _expCtx->setCollationMatchesDefault(collationMatchesDefault);

    auto statusWithArrayFilters = parsedUpdateArrayFilters(
        _expCtx, _request->getArrayFilters(), _request->getNamespaceString());
    if (!statusWithArrayFilters.isOK()) {
        return statusWithArrayFilters.getStatus();
    }
    _arrayFilters = std::move(statusWithArrayFilters.getValue());

    _expCtx->startExpressionCounters();

    maybeTranslateTimeseriesUpdate();

    // We parse the update portion before the query portion because the dispostion of the update
    // may determine whether or not we need to produce a CanonicalQuery at all.  For example, if
    // the update involves the positional-dollar operator, we must have a CanonicalQuery even if
    // it isn't required for query execution.
    parseUpdate();
    Status status = parseQuery();

    _expCtx->initializeReferencedSystemVariables();

    return status;
}

Status ParsedUpdateBase::parseQuery() {
    dassert(!_canonicalQuery.get());

    if (!_timeseriesUpdateQueryExprs && !_driver.needMatchDetails() &&
        isSimpleIdQuery(_request->getQuery())) {
        return Status::OK();
    }

    return parseQueryToCQ();
}

Status ParsedUpdateBase::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    // _timeseriesUpdateQueryExprs may be null even if _isRequestToTimeseries is true. See
    // createTimeseriesWritesQueryExprsIfNecessary() for details.
    auto statusWithCQ = impl::parseWriteQueryToCQ(
        _expCtx->getOperationContext(),
        _expCtx.get(),
        *_extensionsCallback,
        *_request,
        _timeseriesUpdateQueryExprs  ? _timeseriesUpdateQueryExprs->_bucketExpr.get()
            : _isRequestToTimeseries ? getClosedBucketFilteredExpr().get()
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

void ParsedUpdateBase::parseUpdate() {
    _driver.setCollator(_expCtx->getCollator());
    _driver.setLogOp(true);
    _driver.setFromOplogApplication(_request->isFromOplogApplication());

    auto source = _request->source();
    if ((source == OperationSource::kFromMigrate) || _request->getBypassEmptyTsReplacement()) {
        _driver.setBypassEmptyTsReplacement(true);
    }

    // Time-series operations will not result in any documents with dots or dollars fields.
    if (source == OperationSource::kTimeseriesInsert ||
        source == OperationSource::kTimeseriesUpdate) {
        _driver.setSkipDotsDollarsCheck(true);
    }

    _expCtx->setIsParsingPipelineUpdate(true);
    _driver.parse(
        *_modification, _arrayFilters, _request->getUpdateConstants(), _request->isMulti());
    _expCtx->setIsParsingPipelineUpdate(false);
}

PlanYieldPolicy::YieldPolicy ParsedUpdateBase::yieldPolicy() const {
    return _request->isGod() ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                             : _request->getYieldPolicy();
}

bool ParsedUpdateBase::hasParsedQuery() const {
    return _canonicalQuery.get() != nullptr;
}

std::unique_ptr<CanonicalQuery> ParsedUpdateBase::releaseParsedQuery() {
    invariant(_canonicalQuery.get() != nullptr);
    return std::move(_canonicalQuery);
}

const UpdateRequest* ParsedUpdateBase::getRequest() const {
    return _request;
}

UpdateDriver* ParsedUpdateBase::getDriver() {
    return &_driver;
}

bool ParsedUpdateBase::isEligibleForArbitraryTimeseriesUpdate() const {
    return _timeseriesUpdateQueryExprs.get() != nullptr;
}

}  // namespace mongo::impl
