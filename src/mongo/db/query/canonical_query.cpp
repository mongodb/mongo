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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/cst/cst_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo {
namespace {

bool parsingCanProduceNoopMatchNodes(const ExtensionsCallback& extensionsCallback,
                                     MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    return extensionsCallback.hasNoopExtensions() &&
        (allowedFeatures & MatchExpressionParser::AllowedFeatures::kText ||
         allowedFeatures & MatchExpressionParser::AllowedFeatures::kJavascript);
}

}  // namespace

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx,
    std::unique_ptr<FindCommandRequest> findCommand,
    bool explain,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    const ProjectionPolicies& projectionPolicies,
    std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline) {
    tassert(5746107,
            "ntoreturn should not be set on the findCommand",
            findCommand->getNtoreturn() == boost::none);

    auto status = query_request_helper::validateFindCommandRequest(*findCommand);
    if (!status.isOK()) {
        return status;
    }

    std::unique_ptr<CollatorInterface> collator;
    if (!findCommand->getCollation().isEmpty()) {
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(findCommand->getCollation());
        if (!statusWithCollator.isOK()) {
            return statusWithCollator.getStatus();
        }
        collator = std::move(statusWithCollator.getValue());
    }

    // Make MatchExpression.
    boost::intrusive_ptr<ExpressionContext> newExpCtx;
    if (!expCtx.get()) {
        invariant(findCommand->getNamespaceOrUUID().nss());
        newExpCtx = make_intrusive<ExpressionContext>(opCtx,
                                                      std::move(collator),
                                                      *findCommand->getNamespaceOrUUID().nss(),
                                                      findCommand->getLegacyRuntimeConstants(),
                                                      findCommand->getLet());
    } else {
        newExpCtx = expCtx;
        // A collator can enter through both the FindCommandRequest and ExpressionContext arguments.
        // This invariant ensures that both collators are the same because downstream we
        // pull the collator from only one of the ExpressionContext carrier.
        if (collator.get() && expCtx->getCollator()) {
            invariant(CollatorInterface::collatorsMatch(collator.get(), expCtx->getCollator()));
        }
    }

    // Make the CQ we'll hopefully return.
    std::unique_ptr<CanonicalQuery> cq(new CanonicalQuery());
    cq->setExplain(explain);

    StatusWithMatchExpression statusWithMatcher = [&]() -> StatusWithMatchExpression {
        if (getTestCommandsEnabled() && internalQueryEnableCSTParser.load()) {
            try {
                return cst::parseToMatchExpression(
                    findCommand->getFilter(), newExpCtx, extensionsCallback);
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        } else {
            return MatchExpressionParser::parse(
                findCommand->getFilter(), newExpCtx, extensionsCallback, allowedFeatures);
        }
    }();
    if (!statusWithMatcher.isOK()) {
        return statusWithMatcher.getStatus();
    }

    // Stop counting match expressions after they have been parsed to exclude expressions created
    // during optimization and other processing steps.
    newExpCtx->stopExpressionCounters();

    std::unique_ptr<MatchExpression> me = std::move(statusWithMatcher.getValue());

    Status initStatus =
        cq->init(opCtx,
                 std::move(newExpCtx),
                 std::move(findCommand),
                 parsingCanProduceNoopMatchNodes(extensionsCallback, allowedFeatures),
                 std::move(me),
                 projectionPolicies,
                 std::move(pipeline));

    if (!initStatus.isOK()) {
        return initStatus;
    }
    return std::move(cq);
}

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx, const CanonicalQuery& baseQuery, MatchExpression* root) {
    auto findCommand = std::make_unique<FindCommandRequest>(baseQuery.nss());
    BSONObjBuilder builder;
    root->serialize(&builder, true);
    findCommand->setFilter(builder.obj());
    findCommand->setProjection(baseQuery.getFindCommandRequest().getProjection().getOwned());
    findCommand->setSort(baseQuery.getFindCommandRequest().getSort().getOwned());
    findCommand->setCollation(baseQuery.getFindCommandRequest().getCollation().getOwned());
    auto status = query_request_helper::validateFindCommandRequest(*findCommand);
    if (!status.isOK()) {
        return status;
    }

    tassert(5842500,
            "Cannot create a sub-query from an existing CanonicalQuery that carries a non-empty "
            "pipeline",
            baseQuery.pipeline().empty());

    // Make the CQ we'll hopefully return.
    std::unique_ptr<CanonicalQuery> cq(new CanonicalQuery());
    cq->setExplain(baseQuery.getExplain());
    Status initStatus = cq->init(opCtx,
                                 baseQuery.getExpCtx(),
                                 std::move(findCommand),
                                 baseQuery.canHaveNoopMatchNodes(),
                                 root->shallowClone(),
                                 ProjectionPolicies::findProjectionPolicies(),
                                 {} /* an empty pipeline */);

    if (!initStatus.isOK()) {
        return initStatus;
    }
    return std::move(cq);
}

Status CanonicalQuery::init(OperationContext* opCtx,
                            boost::intrusive_ptr<ExpressionContext> expCtx,
                            std::unique_ptr<FindCommandRequest> findCommand,
                            bool canHaveNoopMatchNodes,
                            std::unique_ptr<MatchExpression> root,
                            const ProjectionPolicies& projectionPolicies,
                            std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline) {
    _expCtx = expCtx;
    _findCommand = std::move(findCommand);

    _canHaveNoopMatchNodes = canHaveNoopMatchNodes;
    _enableSlotBasedExecutionEngine = internalQueryEnableSlotBasedExecutionEngine.load();

    auto validStatus = isValid(root.get(), *_findCommand);
    if (!validStatus.isOK()) {
        return validStatus.getStatus();
    }
    auto unavailableMetadata = validStatus.getValue();
    _root = MatchExpression::normalize(std::move(root));
    // The tree must always be valid after normalization.
    dassert(isValid(_root.get(), *_findCommand).isOK());
    if (auto status = isValidNormalized(_root.get()); !status.isOK()) {
        return status;
    }

    {
        // If there is a geo expression, extract it.
        if (auto n = countNodes(_root.get(), MatchExpression::GEO_NEAR)) {
            tassert(1234501, "isValidNormalized should have caught extra GEO_NEAR", n == 1);

            if (_root->matchType() == MatchExpression::GEO_NEAR) {
                std::unique_ptr<MatchExpression> nonGeoNear{
                    static_cast<MatchExpression*>(new AlwaysTrueMatchExpression())};
                std::unique_ptr<GeoNearMatchExpression> geoNear{
                    static_cast<GeoNearMatchExpression*>(_root->shallowClone().release())};
                _splitGeoNear = SplitGeoNear{std::move(nonGeoNear), std::move(geoNear)};
            } else {
                tassert(1234502,
                        "GEO_NEAR can only happen in a top-level AND",
                        _root->matchType() == MatchExpression::AND);

                auto nonGeoNear = _root->shallowClone();
                std::unique_ptr<GeoNearMatchExpression> geoNear;

                for (size_t i = 0; i < nonGeoNear->numChildren(); ++i) {
                    auto& children = *nonGeoNear->getChildVector();
                    if (nonGeoNear->getChild(i)->matchType() == MatchExpression::GEO_NEAR) {
                        // Found it.
                        geoNear.reset(static_cast<GeoNearMatchExpression*>(children[i].release()));
                        children.erase(children.begin() + i);
                        break;
                    }
                }
                tassert(1234503, "Expected a GEO_NEAR in here", geoNear);

                // Removing one branch of an $and may have left us with only one remaining branch.
                if (nonGeoNear->numChildren() == 1) {
                    nonGeoNear = std::move(nonGeoNear->getChildVector()->at(0));
                }

                _splitGeoNear = SplitGeoNear{std::move(nonGeoNear), std::move(geoNear)};
            }
        }
    }

    // Validate the projection if there is one.
    if (!_findCommand->getProjection().isEmpty()) {
        try {
            _proj.emplace(projection_ast::parseAndAnalyze(expCtx,
                                                          _findCommand->getProjection(),
                                                          _root.get(),
                                                          _findCommand->getFilter(),
                                                          projectionPolicies,
                                                          true /* Should optimize? */));

            // Fail if any of the projection's dependencies are unavailable.
            DepsTracker{unavailableMetadata}.requestMetadata(_proj->metadataDeps());
        } catch (const DBException& e) {
            return e.toStatus();
        }

        _metadataDeps = _proj->metadataDeps();
    }

    _pipeline = std::move(pipeline);

    if (_proj && _proj->metadataDeps()[DocumentMetadataFields::kSortKey] &&
        _findCommand->getSort().isEmpty()) {
        return Status(ErrorCodes::BadValue, "cannot use sortKey $meta projection without a sort");
    }

    // If there is a sort, parse it and add any metadata dependencies it induces.
    try {
        initSortPattern(unavailableMetadata);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    // If the 'returnKey' option is set, then the plan should produce index key metadata.
    if (_findCommand->getReturnKey()) {
        _metadataDeps.set(DocumentMetadataFields::kIndexKey);
    }

    return Status::OK();
}

void CanonicalQuery::initSortPattern(QueryMetadataBitSet unavailableMetadata) {
    if (_findCommand->getSort().isEmpty()) {
        return;
    }

    // A $natural sort is really a hint, and should be handled as such. Furthermore, the downstream
    // sort handling code may not expect a $natural sort.
    //
    // We have already validated that if there is a $natural sort and a hint, that the hint
    // also specifies $natural with the same direction. Therefore, it is safe to clear the $natural
    // sort and rewrite it as a $natural hint.
    if (_findCommand->getSort()[query_request_helper::kNaturalSortField]) {
        _findCommand->setHint(_findCommand->getSort().getOwned());
        _findCommand->setSort(BSONObj{});
    }

    if (getTestCommandsEnabled() && internalQueryEnableCSTParser.load()) {
        _sortPattern = cst::parseToSortPattern(_findCommand->getSort(), _expCtx);
    } else {
        _sortPattern = SortPattern{_findCommand->getSort(), _expCtx};
    }
    _metadataDeps |= _sortPattern->metadataDeps(unavailableMetadata);

    // If the results of this query might have to be merged on a remote node, then that node might
    // need the sort key metadata. Request that the plan generates this metadata.
    if (_expCtx->needsMerge) {
        _metadataDeps.set(DocumentMetadataFields::kSortKey);
    }
}

void CanonicalQuery::setCollator(std::unique_ptr<CollatorInterface> collator) {
    auto collatorRaw = collator.get();
    // We must give the ExpressionContext the same collator.
    _expCtx->setCollator(std::move(collator));

    // The collator associated with the match expression tree is now invalid, since we have reset
    // the collator owned by the ExpressionContext.
    _root->setCollator(collatorRaw);
    if (_splitGeoNear) {
        _splitGeoNear->geoNear->setCollator(collatorRaw);
        _splitGeoNear->nonGeoNear->setCollator(collatorRaw);
    }
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

size_t CanonicalQuery::countNodes(const MatchExpression* root, MatchExpression::MatchType type) {
    size_t sum = 0;
    if (type == root->matchType()) {
        sum = 1;
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        sum += countNodes(root->getChild(i), type);
    }
    return sum;
}

boost::optional<GeoNearMatchExpression*> CanonicalQuery::geoNear() const {
    if (_splitGeoNear) {
        return static_cast<GeoNearMatchExpression*>(_splitGeoNear->geoNear.get());
    } else {
        return boost::none;
    }
}

/**
 * Does 'root' have a subtree of type 'subtreeType' with a node of type 'childType' inside?
 */
bool hasNodeInSubtree(const MatchExpression* root,
                      MatchExpression::MatchType childType,
                      MatchExpression::MatchType subtreeType) {
    if (subtreeType == root->matchType()) {
        return QueryPlannerCommon::hasNode(root, childType);
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        if (hasNodeInSubtree(root->getChild(i), childType, subtreeType)) {
            return true;
        }
    }
    return false;
}

StatusWith<QueryMetadataBitSet> CanonicalQuery::isValid(const MatchExpression* root,
                                                        const FindCommandRequest& findCommand) {
    QueryMetadataBitSet unavailableMetadata{};

    // There can only be one TEXT.  If there is a TEXT, it cannot appear inside a NOR.
    //
    // Note that the query grammar (as enforced by the MatchExpression parser) forbids TEXT
    // inside of value-expression clauses like NOT, so we don't check those here.
    size_t numText = countNodes(root, MatchExpression::TEXT);
    if (numText > 1) {
        return Status(ErrorCodes::BadValue, "Too many text expressions");
    } else if (1 == numText) {
        if (hasNodeInSubtree(root, MatchExpression::TEXT, MatchExpression::NOR)) {
            return Status(ErrorCodes::BadValue, "text expression not allowed in nor");
        }
    } else {
        // Text metadata is not available.
        unavailableMetadata.set(DocumentMetadataFields::kTextScore);
    }

    // There can only be one NEAR.  If there is a NEAR, it must be either the root or the root
    // must be an AND and its child must be a NEAR.
    size_t numGeoNear = countNodes(root, MatchExpression::GEO_NEAR);
    if (numGeoNear > 1) {
        return Status(ErrorCodes::BadValue, "Too many geoNear expressions");
    } else if (1 == numGeoNear) {
        // Do nothing, we will perform extra checks in CanonicalQuery::isValidNormalized.
    } else {
        // Geo distance and geo point metadata are unavailable.
        unavailableMetadata |= DepsTracker::kAllGeoNearData;
    }

    const BSONObj& sortObj = findCommand.getSort();
    BSONElement sortNaturalElt = sortObj["$natural"];
    const BSONObj& hintObj = findCommand.getHint();
    BSONElement hintNaturalElt = hintObj["$natural"];

    if (sortNaturalElt && sortObj.nFields() != 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Cannot include '$natural' in compound sort: " << sortObj);
    }

    if (hintNaturalElt && hintObj.nFields() != 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Cannot include '$natural' in compound hint: " << hintObj);
    }

    // NEAR cannot have a $natural sort or $natural hint.
    if (numGeoNear > 0) {
        if (sortNaturalElt) {
            return Status(ErrorCodes::BadValue,
                          "geoNear expression not allowed with $natural sort order");
        }

        if (hintNaturalElt) {
            return Status(ErrorCodes::BadValue,
                          "geoNear expression not allowed with $natural hint");
        }
    }

    // TEXT and NEAR cannot both be in the query.
    if (numText > 0 && numGeoNear > 0) {
        return Status(ErrorCodes::BadValue, "text and geoNear not allowed in same query");
    }

    // TEXT and {$natural: ...} sort order cannot both be in the query.
    if (numText > 0 && sortNaturalElt) {
        return Status(ErrorCodes::BadValue, "text expression not allowed with $natural sort order");
    }

    // TEXT and hint cannot both be in the query.
    if (numText > 0 && !hintObj.isEmpty()) {
        return Status(ErrorCodes::BadValue, "text and hint not allowed in same query");
    }

    // TEXT and tailable are incompatible.
    if (numText > 0 && findCommand.getTailable()) {
        return Status(ErrorCodes::BadValue, "text and tailable cursor not allowed in same query");
    }

    // NEAR and tailable are incompatible.
    if (numGeoNear > 0 && findCommand.getTailable()) {
        return Status(ErrorCodes::BadValue,
                      "Tailable cursors and geo $near cannot be used together");
    }

    // $natural sort order must agree with hint.
    if (sortNaturalElt) {
        if (!hintObj.isEmpty() && !hintNaturalElt) {
            return Status(ErrorCodes::BadValue, "index hint not allowed with $natural sort order");
        }
        if (hintNaturalElt) {
            if (hintNaturalElt.numberInt() != sortNaturalElt.numberInt()) {
                return Status(ErrorCodes::BadValue,
                              "$natural hint must be in the same direction as $natural sort order");
            }
        }
    }

    return unavailableMetadata;
}

Status CanonicalQuery::isValidNormalized(const MatchExpression* root) {
    if (auto numGeoNear = countNodes(root, MatchExpression::GEO_NEAR); numGeoNear > 0) {
        tassert(5705300, "Only one $getNear expression is expected", numGeoNear == 1);

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
            return Status(ErrorCodes::BadValue, "geoNear must be top-level expr");
        }
    }

    return Status::OK();
}

int CanonicalQuery::getOptions() const {
    int options = 0;
    if (_findCommand->getTailable()) {
        options |= QueryOption_CursorTailable;
    }
    if (_findCommand->getAwaitData()) {
        options |= QueryOption_AwaitData;
    }
    if (_findCommand->getNoCursorTimeout()) {
        options |= QueryOption_NoCursorTimeout;
    }
    if (_findCommand->getAllowPartialResults()) {
        options |= QueryOption_PartialResults;
    }
    return options;
}

std::string CanonicalQuery::toString() const {
    str::stream ss;
    ss << "ns=" << _findCommand->getNamespaceOrUUID().nss().value_or(NamespaceString()).ns();

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
    if (_splitGeoNear) {
        ss << "NonGeoNear: " << _splitGeoNear->nonGeoNear->debugString();
        ss << "GeoNear: " << _splitGeoNear->geoNear->debugString();
    }
    ss << "Sort: " << _findCommand->getSort().toString() << '\n';
    ss << "Proj: " << _findCommand->getProjection().toString() << '\n';
    if (!_findCommand->getCollation().isEmpty()) {
        ss << "Collation: " << _findCommand->getCollation().toString() << '\n';
    }
    return ss;
}

std::string CanonicalQuery::toStringShort() const {
    str::stream ss;
    ss << "ns: " << _findCommand->getNamespaceOrUUID().nss().value_or(NamespaceString()).ns()
       << " query: " << _findCommand->getFilter().toString()
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
    return canonical_query_encoder::encode(*this);
}

StatusWith<BSONObj> asAggregationCommand(const CanonicalQuery& cq) {
    BSONObjBuilder aggregationBuilder;

    const FindCommandRequest& findCommand = cq.getFindCommandRequest();

    // The find command will translate away ntoreturn above this layer.
    tassert(5746106, "ntoreturn should not be set in the findCommand", !findCommand.getNtoreturn());

    // First, check if this query has options that are not supported in aggregation.
    if (!findCommand.getMin().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kMinFieldName
                              << " not supported in aggregation."};
    }
    if (!findCommand.getMax().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kMaxFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getReturnKey()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kReturnKeyFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getShowRecordId()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kShowRecordIdFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getTailable()) {
        return {ErrorCodes::InvalidPipelineOperator,
                "Tailable cursors are not supported in aggregation."};
    }
    if (findCommand.getNoCursorTimeout()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kNoCursorTimeoutFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getAllowPartialResults()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kAllowPartialResultsFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getSort()[query_request_helper::kNaturalSortField]) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Sort option " << query_request_helper::kNaturalSortField
                              << " not supported in aggregation."};
    }
    // The aggregation command normally does not support the 'singleBatch' option, but we make a
    // special exception if 'limit' is set to 1.
    if (findCommand.getSingleBatch() && findCommand.getLimit().value_or(0) != 1LL) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kSingleBatchFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getReadOnce()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kReadOnceFieldName
                              << " not supported in aggregation."};
    }

    if (findCommand.getAllowSpeculativeMajorityRead()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option "
                              << FindCommandRequest::kAllowSpeculativeMajorityReadFieldName
                              << " not supported in aggregation."};
    }

    if (findCommand.getRequestResumeToken()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kRequestResumeTokenFieldName
                              << " not supported in aggregation."};
    }

    if (!findCommand.getResumeAfter().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kResumeAfterFieldName
                              << " not supported in aggregation."};
    }

    // Now that we've successfully validated this QR, begin building the aggregation command.
    aggregationBuilder.append("aggregate",
                              findCommand.getNamespaceOrUUID().nss()
                                  ? findCommand.getNamespaceOrUUID().nss()->coll()
                                  : "");

    // Construct an aggregation pipeline that finds the equivalent documents to this query request.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    {
        if (auto geoNear = cq.geoNear()) {
            const auto& args = (*geoNear)->getData();

            BSONObjBuilder geoStage = pipelineBuilder.subobjStart();
            BSONObjBuilder geoArgs = geoStage.subobjStart("$geoNear"_sd);

            geoArgs.append("key"_sd, (*geoNear)->path());

            // $near and $nearSphere both support two different syntaxes:
            // - GeoJSON: {type: Point, coordinates: [x, y]}
            // - legacy coordinate pair: [x, y]
            bool spherical = [&]() {
                // $nearSphere always uses spherical geometry, but $near can use either
                // spherical or planar geometry, depending on how the centroid is specified.
                switch (args.centroid->crs) {
                    case CRS::FLAT:
                        return false;
                    case CRS::SPHERE:
                        return true;
                    case CRS::STRICT_SPHERE:
                        tasserted(5844301,
                                  "CRS::STRICT_SPHERE should not be possible in a "
                                  "$near/$nearSphere query");
                    case CRS::UNSET:
                        tasserted(5844302,
                                  "CRS::UNSET should not be possible in a $near/$nearSphere query");
                }
                tasserted(5844300, "Unhandled enum value for CRS, in $near/$nearSphere query");
            }();
            geoArgs.append("spherical"_sd, spherical);

            if (args.unitsAreRadians || !spherical) {
                // Write the $geoNear as [x, y] coordinates.
                BSONArrayBuilder nearBuilder = geoArgs.subarrayStart("near"_sd);
                nearBuilder.append(args.centroid->oldPoint.x);
                nearBuilder.append(args.centroid->oldPoint.y);
                nearBuilder.doneFast();
            } else {
                // Write the $geoNear as a GeoJSON point.
                BSONObjBuilder nearBuilder = geoArgs.subobjStart("near"_sd);
                nearBuilder.append("type"_sd, "Point"_sd);
                {
                    BSONArrayBuilder coordinates = nearBuilder.subarrayStart("coordinates"_sd);
                    coordinates.append(args.centroid->oldPoint.x);
                    coordinates.append(args.centroid->oldPoint.y);
                    coordinates.doneFast();
                }
                nearBuilder.doneFast();
            }

            if (args.minDistance) {
                geoArgs.append("minDistance"_sd, args.minDistance);
            }
            if (args.maxDistance) {
                geoArgs.append("maxDistance"_sd, args.maxDistance);
            }
            geoArgs.doneFast();
            geoStage.doneFast();
        }
        if (auto filter = cq.rootWithoutGeoNear(); !filter->isTriviallyTrue()) {
            BSONObjBuilder matchBuilder = pipelineBuilder.subobjStart();
            matchBuilder.append("$match"_sd, filter->serialize());
            matchBuilder.doneFast();
        }
    }
    if (!findCommand.getSort().isEmpty()) {
        BSONObjBuilder sortBuilder(pipelineBuilder.subobjStart());
        sortBuilder.append("$sort", findCommand.getSort());
        sortBuilder.doneFast();
    }
    if (findCommand.getSkip()) {
        BSONObjBuilder skipBuilder(pipelineBuilder.subobjStart());
        skipBuilder.append("$skip", *findCommand.getSkip());
        skipBuilder.doneFast();
    }
    if (findCommand.getLimit()) {
        BSONObjBuilder limitBuilder(pipelineBuilder.subobjStart());
        limitBuilder.append("$limit", *findCommand.getLimit());
        limitBuilder.doneFast();
    }
    if (!findCommand.getProjection().isEmpty()) {
        BSONObjBuilder projectBuilder(pipelineBuilder.subobjStart());
        projectBuilder.append("$project", findCommand.getProjection());
        projectBuilder.doneFast();
    }
    pipelineBuilder.doneFast();

    // The aggregation 'cursor' option is always set, regardless of the presence of batchSize.
    BSONObjBuilder batchSizeBuilder(aggregationBuilder.subobjStart("cursor"));
    if (findCommand.getBatchSize()) {
        batchSizeBuilder.append(FindCommandRequest::kBatchSizeFieldName,
                                *findCommand.getBatchSize());
    }
    batchSizeBuilder.doneFast();

    // Other options.
    aggregationBuilder.append("collation", findCommand.getCollation());
    int maxTimeMS = findCommand.getMaxTimeMS() ? static_cast<int>(*findCommand.getMaxTimeMS()) : 0;
    if (maxTimeMS > 0) {
        aggregationBuilder.append(query_request_helper::cmdOptionMaxTimeMS, maxTimeMS);
    }
    if (!findCommand.getHint().isEmpty()) {
        aggregationBuilder.append(FindCommandRequest::kHintFieldName, findCommand.getHint());
    }
    if (findCommand.getReadConcern()) {
        aggregationBuilder.append("readConcern", *findCommand.getReadConcern());
    }
    if (!findCommand.getUnwrappedReadPref().isEmpty()) {
        aggregationBuilder.append(FindCommandRequest::kUnwrappedReadPrefFieldName,
                                  findCommand.getUnwrappedReadPref());
    }
    if (findCommand.getAllowDiskUse()) {
        aggregationBuilder.append(FindCommandRequest::kAllowDiskUseFieldName,
                                  static_cast<bool>(findCommand.getAllowDiskUse()));
    }
    if (findCommand.getLegacyRuntimeConstants()) {
        BSONObjBuilder rtcBuilder(
            aggregationBuilder.subobjStart(FindCommandRequest::kLegacyRuntimeConstantsFieldName));
        findCommand.getLegacyRuntimeConstants()->serialize(&rtcBuilder);
        rtcBuilder.doneFast();
    }
    if (findCommand.getLet()) {
        aggregationBuilder.append(FindCommandRequest::kLetFieldName, *findCommand.getLet());
    }
    return StatusWith<BSONObj>(aggregationBuilder.obj());
}

}  // namespace mongo
