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

#include "mongo/platform/basic.h"

#include "mongo/db/ops/parsed_update.h"

#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/server_options.h"

namespace mongo {

ParsedUpdate::ParsedUpdate(OperationContext* opCtx,
                           const UpdateRequest* request,
                           const ExtensionsCallback& extensionsCallback)
    : _opCtx(opCtx),
      _request(request),
      _expCtx(make_intrusive<ExpressionContext>(
          opCtx, nullptr, _request->getNamespaceString(), _request->getRuntimeConstants())),
      _driver(_expCtx),
      _canonicalQuery(),
      _extensionsCallback(extensionsCallback) {}

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

    if (!_request->getCollation().isEmpty()) {
        auto collator = CollatorFactoryInterface::get(_opCtx->getServiceContext())
                            ->makeFromBSON(_request->getCollation());
        if (!collator.isOK()) {
            return collator.getStatus();
        }
        _expCtx->setCollator(std::move(collator.getValue()));
    }

    auto statusWithArrayFilters =
        parseArrayFilters(_expCtx, _request->getArrayFilters(), _request->getNamespaceString());
    if (!statusWithArrayFilters.isOK()) {
        return statusWithArrayFilters.getStatus();
    }
    _arrayFilters = std::move(statusWithArrayFilters.getValue());

    // We parse the update portion before the query portion because the dispostion of the update
    // may determine whether or not we need to produce a CanonicalQuery at all.  For example, if
    // the update involves the positional-dollar operator, we must have a CanonicalQuery even if
    // it isn't required for query execution.
    parseUpdate();
    Status status = parseQuery();
    if (!status.isOK())
        return status;
    return Status::OK();
}

Status ParsedUpdate::parseQuery() {
    dassert(!_canonicalQuery.get());

    if (!_driver.needMatchDetails() && CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
        return Status::OK();
    }

    return parseQueryToCQ();
}

Status ParsedUpdate::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    // The projection needs to be applied after the update operation, so we do not specify a
    // projection during canonicalization.
    auto qr = std::make_unique<QueryRequest>(_request->getNamespaceString());
    qr->setFilter(_request->getQuery());
    qr->setSort(_request->getSort());
    qr->setExplain(_request->isExplain());
    qr->setHint(_request->getHint());

    // We get the collation off the ExpressionContext because it may contain a collection-default
    // collator if no collation was included in the user's request.
    qr->setCollation(_expCtx->getCollatorBSON());

    // Limit should only used for the findAndModify command when a sort is specified. If a sort
    // is requested, we want to use a top-k sort for efficiency reasons, so should pass the
    // limit through. Generally, a update stage expects to be able to skip documents that were
    // deleted/modified under it, but a limit could inhibit that and give an EOF when the update
    // has not actually updated a document. This behavior is fine for findAndModify, but should
    // not apply to update in general.
    if (!_request->isMulti() && !_request->getSort().isEmpty()) {
        qr->setLimit(1);
    }

    // $expr is not allowed in the query for an upsert, since it is not clear what the equality
    // extraction behavior for $expr should be.
    MatchExpressionParser::AllowedFeatureSet allowedMatcherFeatures =
        MatchExpressionParser::kAllowAllSpecialFeatures;
    if (_request->isUpsert()) {
        allowedMatcherFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
    }

    // If the update request has runtime constants attached to it, pass them to the QueryRequest.
    if (auto& runtimeConstants = _request->getRuntimeConstants()) {
        qr->setRuntimeConstants(*runtimeConstants);
    }

    auto statusWithCQ = CanonicalQuery::canonicalize(
        _opCtx, std::move(qr), _expCtx, _extensionsCallback, allowedMatcherFeatures);
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

    _driver.parse(_request->getUpdateModification(),
                  _arrayFilters,
                  _request->getUpdateConstants(),
                  _request->isMulti());
}

StatusWith<std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>>
ParsedUpdate::parseArrayFilters(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                const std::vector<BSONObj>& rawArrayFiltersIn,
                                const NamespaceString& nss) {
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFiltersOut;
    for (auto rawArrayFilter : rawArrayFiltersIn) {
        auto parsedArrayFilter =
            MatchExpressionParser::parse(rawArrayFilter,
                                         std::move(expCtx),
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures);

        if (!parsedArrayFilter.isOK()) {
            return parsedArrayFilter.getStatus().withContext("Error parsing array filter");
        }
        auto parsedArrayFilterWithPlaceholder =
            ExpressionWithPlaceholder::make(std::move(parsedArrayFilter.getValue()));
        if (!parsedArrayFilterWithPlaceholder.isOK()) {
            return parsedArrayFilterWithPlaceholder.getStatus().withContext(
                "Error parsing array filter");
        }
        auto finalArrayFilter = std::move(parsedArrayFilterWithPlaceholder.getValue());
        auto fieldName = finalArrayFilter->getPlaceholder();
        if (!fieldName) {
            return Status(
                ErrorCodes::FailedToParse,
                "Cannot use an expression without a top-level field name in arrayFilters");
        }
        if (arrayFiltersOut.find(*fieldName) != arrayFiltersOut.end()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Found multiple array filters with the same top-level field name "
                              << *fieldName);
        }

        arrayFiltersOut[*fieldName] = std::move(finalArrayFilter);
    }

    return std::move(arrayFiltersOut);
}

PlanExecutor::YieldPolicy ParsedUpdate::yieldPolicy() const {
    return _request->isGod() ? PlanExecutor::NO_YIELD : _request->getYieldPolicy();
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

void ParsedUpdate::setCollator(std::unique_ptr<CollatorInterface> collator) {
    auto* rawCollator = collator.get();

    if (_canonicalQuery) {
        _canonicalQuery->setCollator(std::move(collator));
    } else {
        _expCtx->setCollator(std::move(collator));
    }

    _driver.setCollator(rawCollator);

    for (auto&& arrayFilter : _arrayFilters) {
        arrayFilter.second->getFilter()->setCollator(rawCollator);
    }
}

}  // namespace mongo
