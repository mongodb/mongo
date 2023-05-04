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

#include "mongo/db/ops/parsed_update.h"

#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/ops/parsed_update_array_filters.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"

namespace mongo {

// Note: The caller should hold a lock on the 'collection' so that it can stay alive until the end
// of the ParsedUpdate's lifetime.
ParsedUpdate::ParsedUpdate(OperationContext* opCtx,
                           const UpdateRequest* request,
                           const ExtensionsCallback& extensionsCallback,
                           const CollectionPtr& collection,
                           bool forgoOpCounterIncrements,
                           bool isRequestToTimeseries)
    : _opCtx(opCtx),
      _request(request),
      _expCtx(make_intrusive<ExpressionContext>(
          opCtx,
          nullptr,
          _request->getNamespaceString(),
          _request->getLegacyRuntimeConstants(),
          _request->getLetParameters(),
          allowDiskUseByDefault.load(),  // allowDiskUse
          true,  // mayDbProfile. We pass 'true' here conservatively. In the future we may
          // change this.
          request->explain())),
      _driver(_expCtx),
      _modification(
          std::make_unique<write_ops::UpdateModification>(_request->getUpdateModification())),
      _canonicalQuery(),
      _extensionsCallback(extensionsCallback),
      _collection(collection),
      _timeseriesUpdateQueryExprs(isRequestToTimeseries
                                      ? createTimeseriesWritesQueryExprsIfNecessary(
                                            feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                                                serverGlobalParams.featureCompatibility),
                                            collection)
                                      : nullptr) {
    if (forgoOpCounterIncrements) {
        _expCtx->enabledCounters = false;
    }
    _expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
}

Status ParsedUpdate::parseRequest() {
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
                constants && (*constants)["new"_sd].type() == BSONType::Object);
    }

    // It is invalid to request that a ProjectionStage be applied to the UpdateStage if the
    // UpdateStage would not return any document.
    invariant(_request->getProj().isEmpty() || _request->shouldReturnAnyDocs());

    auto [collatorToUse, collationMatchesDefault] =
        resolveCollator(_opCtx, _request->getCollation(), _collection);
    _expCtx->setCollator(std::move(collatorToUse));
    _expCtx->collationMatchesDefault = collationMatchesDefault;

    auto statusWithArrayFilters = parsedUpdateArrayFilters(
        _expCtx, _request->getArrayFilters(), _request->getNamespaceString());
    if (!statusWithArrayFilters.isOK()) {
        return statusWithArrayFilters.getStatus();
    }
    _arrayFilters = std::move(statusWithArrayFilters.getValue());

    if (_timeseriesUpdateQueryExprs) {
        tassert(7314206, "collection must not be null", _collection);

        // With timeseries updates, we have to parse the query first to determine whether we will be
        // performing the update directly on the bucket document (and therefore need to translate
        // the update modifications), or if we will unpack the measurements first and be able to use
        // the modifications as-is.
        // (We know that we will always need to produce a query for this case because timeseries is
        // not eligible for idhack.)
        Status status = parseQueryToCQ();
        if (!status.isOK()) {
            return status;
        }

        if (_request->isMulti() && !_timeseriesUpdateQueryExprs->_residualExpr) {
            // If we don't have a residual predicate and this is not a single update, we might be
            // able to perform this update directly on the buckets collection. Attempt to translate
            // the update modification accordingly, if it succeeds, bail out and do a direct write.
            // If we can't translate it (due to referencing data fields), go ahead with the
            // arbitrary updates path.
            const auto& timeseriesOptions = _collection->getTimeseriesOptions();
            auto swModification =
                timeseries::translateUpdate(*_modification, timeseriesOptions->getMetaField());
            if (swModification.isOK()) {
                _modification =
                    std::make_unique<write_ops::UpdateModification>(swModification.getValue());
                _timeseriesUpdateQueryExprs = nullptr;
            }
        }
        parseUpdate();
        return Status::OK();
    }

    // We parse the update portion before the query portion because the dispostion of the update
    // may determine whether or not we need to produce a CanonicalQuery at all.  For example, if
    // the update involves the positional-dollar operator, we must have a CanonicalQuery even if
    // it isn't required for query execution.
    parseUpdate();
    Status status = parseQuery();

    // After parsing to detect if $$USER_ROLES is referenced in the query, set the value of
    // $$USER_ROLES for the update.
    _expCtx->setUserRoles();

    if (!status.isOK())
        return status;
    return Status::OK();
}

Status ParsedUpdate::parseQuery() {
    dassert(!_canonicalQuery.get());
    tassert(7314204,
            "timeseries updates should always require a canonical query to be parsed",
            !_timeseriesUpdateQueryExprs);

    if (!_driver.needMatchDetails() && CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
        return Status::OK();
    }

    return parseQueryToCQ();
}

Status ParsedUpdate::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    // The projection needs to be applied after the update operation, so we do not specify a
    // projection during canonicalization.
    auto findCommand = std::make_unique<FindCommandRequest>(_request->getNamespaceString());

    _expCtx->startExpressionCounters();

    if (_timeseriesUpdateQueryExprs) {
        // If we're updating documents in a time-series collection, splits the match expression
        // into a bucket-level match expression and a residual expression so that we can push down
        // the bucket-level match expression to the system bucket collection scan or fetch/ixscan.
        *_timeseriesUpdateQueryExprs = timeseries::getMatchExprsForWrites(
            _expCtx, *_collection->getTimeseriesOptions(), _request->getQuery());

        // At this point, we parsed user-provided match expression. After this point, the new
        // canonical query is internal to the bucket SCAN or FETCH and will have additional internal
        // match expression. We do not need to track the internal match expression counters and so
        // we stop the counters because we do not want to count the internal match expression.
        _expCtx->stopExpressionCounters();

        // At least, the bucket-level filter must contain the closed bucket filter.
        tassert(7314200,
                "Bucket-level filter must not be null",
                _timeseriesUpdateQueryExprs->_bucketExpr);
        findCommand->setFilter(_timeseriesUpdateQueryExprs->_bucketExpr->serialize());
    } else {
        findCommand->setFilter(_request->getQuery());
    }
    findCommand->setSort(_request->getSort());
    findCommand->setHint(_request->getHint());
    findCommand->setCollation(_request->getCollation().getOwned());

    // Limit should only used for the findAndModify command when a sort is specified. If a sort
    // is requested, we want to use a top-k sort for efficiency reasons, so should pass the
    // limit through. Generally, a update stage expects to be able to skip documents that were
    // deleted/modified under it, but a limit could inhibit that and give an EOF when the update
    // has not actually updated a document. This behavior is fine for findAndModify, but should
    // not apply to update in general.
    if (!_request->isMulti() && !_request->getSort().isEmpty()) {
        // TODO: Due to the complexity which is related to the efficient sort support, we don't
        // support yet findAndModify with a query and sort but it should not be impossible.
        // This code assumes that in findAndModify code path, the parsed update constructor should
        // be called with source == kTimeseriesUpdate for a time-series collection.
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a findAndModify with a query and sort on a time-series collection.",
                !_timeseriesUpdateQueryExprs);
        findCommand->setLimit(1);
    }

    // $expr is not allowed in the query for an upsert, since it is not clear what the equality
    // extraction behavior for $expr should be.
    MatchExpressionParser::AllowedFeatureSet allowedMatcherFeatures =
        MatchExpressionParser::kAllowAllSpecialFeatures;
    if (_request->isUpsert()) {
        allowedMatcherFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
    }

    // If the update request has runtime constants or let parameters attached to it, pass them to
    // the FindCommandRequest.
    if (auto& runtimeConstants = _request->getLegacyRuntimeConstants()) {
        findCommand->setLegacyRuntimeConstants(*runtimeConstants);
    }
    if (auto& letParams = _request->getLetParameters()) {
        findCommand->setLet(*letParams);
    }

    auto statusWithCQ = CanonicalQuery::canonicalize(_opCtx,
                                                     std::move(findCommand),
                                                     static_cast<bool>(_request->explain()),
                                                     _expCtx,
                                                     _extensionsCallback,
                                                     allowedMatcherFeatures);
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

void ParsedUpdate::parseUpdate() {
    _driver.setCollator(_expCtx->getCollator());
    _driver.setLogOp(true);
    _driver.setFromOplogApplication(_request->isFromOplogApplication());
    // Time-series operations will not result in any documents with dots or dollars fields.
    if (auto source = _request->source(); source == OperationSource::kTimeseriesInsert ||
        source == OperationSource::kTimeseriesUpdate) {
        _driver.setSkipDotsDollarsCheck(true);
    }
    _expCtx->isParsingPipelineUpdate = true;
    _driver.parse(
        *_modification, _arrayFilters, _request->getUpdateConstants(), _request->isMulti());
    _expCtx->isParsingPipelineUpdate = false;
}

PlanYieldPolicy::YieldPolicy ParsedUpdate::yieldPolicy() const {
    return _request->isGod() ? PlanYieldPolicy::YieldPolicy::NO_YIELD : _request->getYieldPolicy();
}

bool ParsedUpdate::hasParsedQuery() const {
    return _canonicalQuery.get() != nullptr;
}

std::unique_ptr<CanonicalQuery> ParsedUpdate::releaseParsedQuery() {
    invariant(_canonicalQuery.get() != nullptr);
    return std::move(_canonicalQuery);
}

const UpdateRequest* ParsedUpdate::getRequest() const {
    return _request;
}

UpdateDriver* ParsedUpdate::getDriver() {
    return &_driver;
}

bool ParsedUpdate::isEligibleForArbitraryTimeseriesUpdate() const {
    return _timeseriesUpdateQueryExprs.get() != nullptr;
}

}  // namespace mongo
