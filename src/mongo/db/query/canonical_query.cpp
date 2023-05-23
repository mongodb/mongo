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

#include "mongo/db/query/canonical_query.h"

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/cst/cst_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

boost::optional<size_t> loadMaxParameterCount() {
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
    std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline,
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
                            std::move(pipeline),
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
            std::move(expCtx), std::move(parsedFind), explain, std::move(pipeline), isCountLike);
    }
}

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<ParsedFindCommand> parsedFind,
    bool explain,
    std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline,
    bool isCountLike) {

    // Make the CQ we'll hopefully return.
    auto cq = std::make_unique<CanonicalQuery>();
    cq->setExplain(explain);
    if (auto initStatus =
            cq->init(std::move(expCtx), std::move(parsedFind), std::move(pipeline), isCountLike);
        !initStatus.isOK()) {
        return initStatus;
    }
    return {std::move(cq)};
}

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx, const CanonicalQuery& baseQuery, MatchExpression* root) {
    auto findCommand = std::make_unique<FindCommandRequest>(baseQuery.nss());
    findCommand->setFilter(root->serialize());
    findCommand->setProjection(baseQuery.getFindCommandRequest().getProjection().getOwned());
    findCommand->setSort(baseQuery.getFindCommandRequest().getSort().getOwned());
    findCommand->setCollation(baseQuery.getFindCommandRequest().getCollation().getOwned());

    // Make the CQ we'll hopefully return.
    auto cq = std::make_unique<CanonicalQuery>();
    cq->setExplain(baseQuery.getExplain());
    auto swParsedFind = parsed_find_command::parse(baseQuery.getExpCtx(), std::move(findCommand));
    if (!swParsedFind.isOK()) {
        return swParsedFind.getStatus();
    }
    auto initStatus = cq->init(baseQuery.getExpCtx(),
                               std::move(swParsedFind.getValue()),
                               {} /* an empty pipeline */,
                               baseQuery.isCountLike());
    invariant(initStatus.isOK());
    return {std::move(cq)};
}

Status CanonicalQuery::init(boost::intrusive_ptr<ExpressionContext> expCtx,
                            std::unique_ptr<ParsedFindCommand> parsedFind,
                            std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline,
                            bool isCountLike) {
    _expCtx = expCtx;
    _findCommand = std::move(parsedFind->findCommandRequest);

    _forceClassicEngine = ServerParameterSet::getNodeParameterSet()
                              ->get<QueryFrameworkControl>("internalQueryFrameworkControl")
                              ->_data.get() == QueryFrameworkControlEnum::kForceClassicEngine;

    _root = MatchExpression::normalize(std::move(parsedFind->filter));
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
                                                          _root.get(),
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
    _pipeline = std::move(pipeline);
    _isCountLike = isCountLike;


    // If caching is disabled, do not perform any autoparameterization.
    if (!internalQueryDisablePlanCache.load()) {
        const bool hasNoTextNodes =
            !QueryPlannerCommon::hasNode(_root.get(), MatchExpression::TEXT);
        if (hasNoTextNodes) {
            // When the SBE plan cache is enabled, we auto-parameterize queries in the hopes of
            // caching a parameterized plan. Here we add parameter markers to the appropriate match
            // expression leaf nodes.
            _inputParamIdToExpressionMap =
                MatchExpression::parameterize(_root.get(), loadMaxParameterCount());
        } else {
            LOGV2_DEBUG(6579310,
                        5,
                        "The query was not auto-parameterized since its match expression tree "
                        "contains TEXT nodes");
        }
    }
    // The tree must always be valid after normalization.
    dassert(parsed_find_command::isValid(_root.get(), *_findCommand).isOK());
    if (auto status = isValidNormalized(_root.get()); !status.isOK()) {
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
    _root->setCollator(collatorRaw);
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
        ss << "ns="
           << _findCommand->getNamespaceOrUUID()
                  .nss()
                  .value_or(NamespaceString())
                  .toStringForErrorMsg();
    } else {
        ss << "ns=" << _findCommand->getNamespaceOrUUID().nss().value_or(NamespaceString()).ns();
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
    ss << "Tree: " << _root->debugString();
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
        ss << "ns: "
           << _findCommand->getNamespaceOrUUID()
                  .nss()
                  .value_or(NamespaceString())
                  .toStringForErrorMsg();
    } else {
        ss << "ns: " << _findCommand->getNamespaceOrUUID().nss().value_or(NamespaceString()).ns();
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
}  // namespace mongo
