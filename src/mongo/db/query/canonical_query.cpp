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


#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <cstdint>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_decorations.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/server_parameter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/str.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

boost::optional<size_t> loadMaxMatchExpressionParams() {
    auto value = internalQueryAutoParameterizationMaxParameterCount.load();
    if (value > 0) {
        return value;
    }

    return boost::none;
}

}  // namespace

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx,
    std::unique_ptr<FindCommandRequest> findCommand,
    bool explain,
    const boost::intrusive_ptr<ExpressionContext>& givenExpCtx,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    const ProjectionPolicies& projectionPolicies,
    std::vector<std::unique_ptr<InnerPipelineStageInterface>> cqPipeline,
    bool isCountLike) {
    if (givenExpCtx) {
        // Caller provided an ExpressionContext, let's go ahead and use that.
        auto swParsedFind = parsed_find_command::parse(givenExpCtx,
                                                       std::move(findCommand),
                                                       extensionsCallback,
                                                       allowedFeatures,
                                                       projectionPolicies);
        if (!swParsedFind.isOK()) {
            return swParsedFind.getStatus();
        }
        return canonicalize(std::move(givenExpCtx),
                            std::move(swParsedFind.getValue()),
                            explain,
                            std::move(cqPipeline),
                            isCountLike);
    } else {
        // No ExpressionContext provided, let's call the override that makes one for us.
        auto swResults = parsed_find_command::parse(
            opCtx, std::move(findCommand), extensionsCallback, allowedFeatures, projectionPolicies);
        if (!swResults.isOK()) {
            return swResults.getStatus();
        }
        auto&& [expCtx, parsedFind] = std::move(swResults.getValue());
        return canonicalize(
            std::move(expCtx), std::move(parsedFind), explain, std::move(cqPipeline), isCountLike);
    }
}

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<ParsedFindCommand> parsedFind,
    bool explain,
    std::vector<std::unique_ptr<InnerPipelineStageInterface>> cqPipeline,
    bool isCountLike) {

    // Make the CQ we'll hopefully return.
    auto cq = std::make_unique<CanonicalQuery>();
    cq->setExplain(explain);
    if (auto initStatus = cq->initCq(
            std::move(expCtx), std::move(parsedFind), std::move(cqPipeline), isCountLike);
        !initStatus.isOK()) {
        return initStatus;
    }
    return {std::move(cq)};
}

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx, const CanonicalQuery& baseQuery, MatchExpression* matchExpr) {
    auto findCommand = std::make_unique<FindCommandRequest>(baseQuery.nss());
    findCommand->setFilter(matchExpr->serialize());
    findCommand->setProjection(baseQuery.getFindCommandRequest().getProjection().getOwned());
    findCommand->setSort(baseQuery.getFindCommandRequest().getSort().getOwned());
    findCommand->setCollation(baseQuery.getFindCommandRequest().getCollation().getOwned());

    // Make the CQ we'll hopefully return.
    auto cq = std::make_unique<CanonicalQuery>();
    cq->setExplain(baseQuery.getExplain());
    auto swParsedFind = ParsedFindCommand::withExistingFilter(
        baseQuery.getExpCtx(),
        baseQuery.getCollator() ? baseQuery.getCollator()->clone() : nullptr,
        matchExpr->clone(),
        std::move(findCommand));
    if (!swParsedFind.isOK()) {
        return swParsedFind.getStatus();
    }
    auto initStatus = cq->initCq(baseQuery.getExpCtx(),
                                 std::move(swParsedFind.getValue()),
                                 {} /* an empty cqPipeline */,
                                 baseQuery.isCountLike());
    invariant(initStatus.isOK());
    return {std::move(cq)};
}

Status CanonicalQuery::initCq(boost::intrusive_ptr<ExpressionContext> expCtx,
                              std::unique_ptr<ParsedFindCommand> parsedFind,
                              std::vector<std::unique_ptr<InnerPipelineStageInterface>> cqPipeline,
                              bool isCountLike) {
    _expCtx = expCtx;
    _findCommand = std::move(parsedFind->findCommandRequest);

    _forceClassicEngine =
        QueryKnobConfiguration::decoration(expCtx->opCtx).getInternalQueryFrameworkControlForOp() ==
        QueryFrameworkControlEnum::kForceClassicEngine;

    _primaryMatchExpression = MatchExpression::normalize(std::move(parsedFind->filter));
    if (parsedFind->proj) {
        if (parsedFind->proj->requiresMatchDetails()) {
            // Sadly, in some cases the match details cannot be generated from the unoptimized
            // MatchExpression. For example, a rooted-$or of equalities won't work to produce the
            // details, but if you optimize that query to an $in, it will work. If we were starting
            // from scratch, we may disallow this. But it has already been released as working so we
            // will keep it so, and here have to re-parse the projection using the new, normalized
            // MatchExpression, before we save this projection for later execution.
            _proj.emplace(projection_ast::parseAndAnalyze(expCtx,
                                                          _findCommand->getProjection(),
                                                          _primaryMatchExpression.get(),
                                                          _findCommand->getFilter(),
                                                          *parsedFind->savedProjectionPolicies,
                                                          true /* optimize */));
        } else {
            _proj.emplace(std::move(*parsedFind->proj));
            _proj->optimize();
        }
    }
    if (parsedFind->sort) {
        _sortPattern = std::move(parsedFind->sort);
    }
    _cqPipeline = std::move(cqPipeline);
    _isCountLike = isCountLike;

    // Perform SBE auto-parameterization if there is not already a reason not to.
    _disablePlanCache = internalQueryDisablePlanCache.load();
    _maxMatchExpressionParams = loadMaxMatchExpressionParams();
    if (expCtx->sbeCompatibility != SbeCompatibility::notCompatible &&
        shouldParameterizeSbe(_primaryMatchExpression.get())) {
        // When the SBE plan cache is enabled, we auto-parameterize queries in the hopes of caching
        // a parameterized plan. Here we add parameter markers to the appropriate match expression
        // leaf nodes unless it has too many predicates. If it did not actually get parameterized,
        // we mark the query as uncacheable for SBE to avoid plan cache flooding.
        bool parameterized;
        _inputParamIdToExpressionMap = MatchExpression::parameterize(
            _primaryMatchExpression.get(), _maxMatchExpressionParams, 0, &parameterized);
        if (!parameterized) {
            // Avoid plan cache flooding by not fully parameterized plans.
            setUncacheableSbe();
        }
    }
    // The tree must always be valid after normalization.
    dassert(parsed_find_command::isValid(_primaryMatchExpression.get(), *_findCommand).isOK());
    if (auto status = isValidNormalized(_primaryMatchExpression.get()); !status.isOK()) {
        return status;
    }

    if (_proj) {
        _metadataDeps = _proj->metadataDeps();

        if (_proj->metadataDeps()[DocumentMetadataFields::kSortKey] &&
            _findCommand->getSort().isEmpty()) {
            return {ErrorCodes::BadValue, "cannot use sortKey $meta projection without a sort"};
        }
    }

    if (_sortPattern) {
        // Be sure to track and add any metadata dependencies from the sort (e.g. text score).
        _metadataDeps |= _sortPattern->metadataDeps(parsedFind->unavailableMetadata);

        // If the results of this query might have to be merged on a remote node, then that node
        // might need the sort key metadata. Request that the plan generates this metadata.
        if (_expCtx->needsMerge) {
            _metadataDeps.set(DocumentMetadataFields::kSortKey);
        }
    }

    // If the 'returnKey' option is set, then the plan should produce index key metadata.
    if (_findCommand->getReturnKey()) {
        _metadataDeps.set(DocumentMetadataFields::kIndexKey);
    }
    return Status::OK();
}

void CanonicalQuery::setCollator(std::unique_ptr<CollatorInterface> collator) {
    auto collatorRaw = collator.get();
    // We must give the ExpressionContext the same collator.
    _expCtx->setCollator(std::move(collator));

    // The collator associated with the match expression tree is now invalid, since we have reset
    // the collator owned by the ExpressionContext.
    _primaryMatchExpression->setCollator(collatorRaw);
}

// static
bool CanonicalQuery::isSimpleIdQuery(const BSONObj& query) {
    bool hasID = false;

    BSONObjIterator it(query);
    while (it.more()) {
        BSONElement elt = it.next();
        if (elt.fieldNameStringData() == "_id") {
            // Verify that the query on _id is a simple equality.
            hasID = true;

            if (elt.type() == Object) {
                // If the value is an object, it can't have a query operator
                // (must be a literal object match).
                if (elt.Obj().firstElementFieldName()[0] == '$') {
                    return false;
                }
            } else if (!Indexability::isExactBoundsGenerating(elt)) {
                // The _id fild cannot be something like { _id : { $gt : ...
                // But it can be BinData.
                return false;
            }
        } else {
            return false;
        }
    }

    return hasID;
}

Status CanonicalQuery::isValidNormalized(const MatchExpression* root) {
    if (auto numGeoNear = QueryPlannerCommon::countNodes(root, MatchExpression::GEO_NEAR);
        numGeoNear > 0) {
        tassert(5705300, "Only one geo $near expression is expected", numGeoNear == 1);

        auto topLevel = false;
        if (MatchExpression::GEO_NEAR == root->matchType()) {
            topLevel = true;
        } else if (MatchExpression::AND == root->matchType()) {
            for (size_t i = 0; i < root->numChildren(); ++i) {
                if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                    topLevel = true;
                    break;
                }
            }
        }

        if (!topLevel) {
            return Status(ErrorCodes::BadValue, "geo $near must be top-level expr");
        }
    }

    return Status::OK();
}

std::string CanonicalQuery::toString(bool forErrMsg) const {
    str::stream ss;
    if (forErrMsg) {
        ss << "ns=" << _findCommand->getNamespaceOrUUID().toStringForErrorMsg();
    } else {
        ss << "ns=" << toStringForLogging(_findCommand->getNamespaceOrUUID());
    }

    if (_findCommand->getBatchSize()) {
        ss << " batchSize=" << *_findCommand->getBatchSize();
    }

    if (_findCommand->getLimit()) {
        ss << " limit=" << *_findCommand->getLimit();
    }

    if (_findCommand->getSkip()) {
        ss << " skip=" << *_findCommand->getSkip();
    }

    // The expression tree puts an endl on for us.
    ss << "Tree: " << _primaryMatchExpression->debugString();
    ss << "Sort: " << _findCommand->getSort().toString() << '\n';
    ss << "Proj: " << _findCommand->getProjection().toString() << '\n';
    if (!_findCommand->getCollation().isEmpty()) {
        ss << "Collation: " << _findCommand->getCollation().toString() << '\n';
    }
    return ss;
}

std::string CanonicalQuery::toStringShort(bool forErrMsg) const {
    str::stream ss;
    if (forErrMsg) {
        ss << "ns: " << _findCommand->getNamespaceOrUUID().toStringForErrorMsg();
    } else {
        ss << "ns: " << toStringForLogging(_findCommand->getNamespaceOrUUID());
    }

    ss << " query: " << _findCommand->getFilter().toString()
       << " sort: " << _findCommand->getSort().toString()
       << " projection: " << _findCommand->getProjection().toString();

    if (!_findCommand->getCollation().isEmpty()) {
        ss << " collation: " << _findCommand->getCollation().toString();
    }

    if (_findCommand->getBatchSize()) {
        ss << " batchSize: " << *_findCommand->getBatchSize();
    }

    if (_findCommand->getLimit()) {
        ss << " limit: " << *_findCommand->getLimit();
    }

    if (_findCommand->getSkip()) {
        ss << " skip: " << *_findCommand->getSkip();
    }

    return ss;
}

CanonicalQuery::QueryShapeString CanonicalQuery::encodeKey() const {
    return (!_forceClassicEngine && _sbeCompatible) ? canonical_query_encoder::encodeSBE(*this)
                                                    : canonical_query_encoder::encodeClassic(*this);
}

CanonicalQuery::QueryShapeString CanonicalQuery::encodeKeyForPlanCacheCommand() const {
    return canonical_query_encoder::encodeForPlanCacheCommand(*this);
}

bool CanonicalQuery::shouldParameterizeSbe(MatchExpression* matchExpr) const {
    if (_disablePlanCache || _isUncacheableSbe ||
        QueryPlannerCommon::hasNode(matchExpr, MatchExpression::TEXT)) {
        return false;
    }
    return true;
}
}  // namespace mongo
