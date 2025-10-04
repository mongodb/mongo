/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/storage/storage_parameters_gen.h"

#include <memory>

namespace mongo {
class DeleteRequest;
class UpdateRequest;

/**
 * Query for timeseries arbitrary writes should be split into two parts: bucket expression and
 * residual expression. The bucket expression is used to find the buckets and the residual
 * expression is used to filter the documents in the buckets.
 */
struct TimeseriesWritesQueryExprs {
    // The bucket-level match expression.
    std::unique_ptr<MatchExpression> _bucketExpr = nullptr;

    // The residual expression which is applied to materialized measurements after splitting out
    // bucket-level match expressions.
    std::unique_ptr<MatchExpression> _residualExpr = nullptr;
};

/**
 * Creates a TimeseriesWritesQueryExprs object if the collection is a time-series collection and
 * the related feature flag is enabled.
 */
inline std::unique_ptr<TimeseriesWritesQueryExprs> createTimeseriesWritesQueryExprsIfNecessary(
    bool featureEnabled, const CollectionPtr& collection) {
    if (featureEnabled && collection && collection->getTimeseriesOptions()) {
        return std::make_unique<TimeseriesWritesQueryExprs>();
    } else {
        return nullptr;
    }
}

template <typename T>
concept IsDeleteOrUpdateRequest =
    std::is_same_v<T, UpdateRequest> || std::is_same_v<T, DeleteRequest>;

namespace impl {
/**
 * Parses the filter of 'request'or the given filter (if given) to a CanonicalQuery. This does a
 * direct transformation and doesn't do any special handling, e.g. for timeseries.
 */
template <typename T>
requires IsDeleteOrUpdateRequest<T>
StatusWith<std::unique_ptr<CanonicalQuery>> parseWriteQueryToCQ(
    OperationContext* opCtx,
    ExpressionContext* expCtx,
    const ExtensionsCallback& extensionsCallback,
    const T& request,
    const MatchExpression* rewrittenFilter = nullptr) {
    // The projection needs to be applied after the delete/update operation, so we do not
    // specify a projection during canonicalization.
    auto findCommand = std::make_unique<FindCommandRequest>(request.getNsString());

    if (rewrittenFilter) {
        findCommand->setFilter(rewrittenFilter->serialize());
    } else {
        findCommand->setFilter(request.getQuery());
    }
    findCommand->setSort(request.getSort());
    findCommand->setHint(request.getHint());
    findCommand->setCollation(request.getCollation().getOwned());

    // Limit should only used for the findAndModify command when a sort is specified. If a sort
    // is requested, we want to use a top-k sort for efficiency reasons, so should pass the
    // limit through. Generally, a update stage expects to be able to skip documents that were
    // deleted/modified under it, but a limit could inhibit that and give an EOF when the
    // delete/update has not actually delete/updated a document. This behavior is fine for
    // findAndModify, but should not apply to delete/update in general.
    if (!request.getMulti() && !request.getSort().isEmpty()) {
        findCommand->setLimit(1);
    }

    MatchExpressionParser::AllowedFeatureSet allowedMatcherFeatures =
        MatchExpressionParser::kAllowAllSpecialFeatures;
    if constexpr (std::is_same_v<T, UpdateRequest>) {
        // $expr is not allowed in the query for an upsert, since it is not clear what the
        // equality extraction behavior for $expr should be.
        if (request.isUpsert()) {
            allowedMatcherFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
        }
    }

    // If the delete/update request has runtime constants or let parameters attached to it, pass
    // them to the FindCommandRequest.
    if (auto& runtimeConstants = request.getLegacyRuntimeConstants()) {
        findCommand->setLegacyRuntimeConstants(*runtimeConstants);
    }
    if (auto& letParams = request.getLet()) {
        findCommand->setLet(*letParams);
    }
    auto expCtxForCq = [&]() {
        if (expCtx) {
            return boost::intrusive_ptr<ExpressionContext>(expCtx);
        }

        return ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build();
    }();
    return CanonicalQuery::make(
        {.expCtx = std::move(expCtxForCq),
         .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                               .extensionsCallback = extensionsCallback,
                                               .allowedFeatures = allowedMatcherFeatures}});
}
}  // namespace impl

template <typename T>
requires IsDeleteOrUpdateRequest<T>
StatusWith<std::unique_ptr<CanonicalQuery>> parseWriteQueryToCQ(
    OperationContext* opCtx,
    ExpressionContext* expCtx,
    const T& request,
    const MatchExpression* rewrittenFilter = nullptr) {
    return impl::parseWriteQueryToCQ<T>(opCtx,
                                        expCtx,
                                        ExtensionsCallbackReal(opCtx, &request.getNsString()),
                                        request,
                                        rewrittenFilter);
}
}  // namespace mongo
