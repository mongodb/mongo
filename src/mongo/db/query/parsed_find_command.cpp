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

#include "mongo/db/query/parsed_find_command.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/logv2/log.h"

namespace mongo::parsed_find_command {

namespace {
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

SortPattern initSortPattern(const std::unique_ptr<FindCommandRequest>& findCommand,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // A $natural sort is really a hint, and should be handled as such. Furthermore, the downstream
    // sort handling code may not expect a $natural sort.
    //
    // We have already validated that if there is a $natural sort and a hint, that the hint
    // also specifies $natural with the same direction. Therefore, it is safe to clear the $natural
    // sort and rewrite it as a $natural hint.
    if (findCommand->getSort()[query_request_helper::kNaturalSortField]) {
        findCommand->setHint(findCommand->getSort().getOwned());
        findCommand->setSort(BSONObj{});
    }

    return {findCommand->getSort(), expCtx};
}

std::unique_ptr<CollatorInterface> resolveCollator(
    OperationContext* opCtx, const std::unique_ptr<FindCommandRequest>& findCommand) {
    if (!findCommand->getCollation().isEmpty()) {
        return uassertStatusOKWithContext(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(findCommand->getCollation()),
                                          "unable to parse collation");
    }
    return nullptr;
}

StatusWith<std::unique_ptr<ParsedFindCommand>> parseWithValidatedCollator(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<FindCommandRequest> findCommand,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    const ProjectionPolicies& projectionPolicies) {
    auto out = std::make_unique<ParsedFindCommand>();

    if (auto status = query_request_helper::validateFindCommandRequest(*findCommand);
        !status.isOK()) {
        return status;
    }

    // Parse the MatchExpression.
    auto statusWithMatcher = MatchExpressionParser::parse(
        findCommand->getFilter(), expCtx, extensionsCallback, allowedFeatures);
    if (!statusWithMatcher.isOK()) {
        return statusWithMatcher.getStatus();
    }

    out->filter = std::move(statusWithMatcher.getValue());
    // Stop counting expressions after they have been parsed to exclude expressions created
    // during optimization and other processing steps.
    expCtx->stopExpressionCounters();

    // Verify the filter follows certain rules like there must be at most one text clause.
    auto swMeta = isValid(out->filter.get(), *findCommand);
    if (!swMeta.isOK()) {
        return swMeta.getStatus();
    }
    out->unavailableMetadata = swMeta.getValue();

    // Validate the projection if there is one.
    if (!findCommand->getProjection().isEmpty()) {
        try {
            out->savedProjectionPolicies.emplace(projectionPolicies);
            out->proj.emplace(projection_ast::parseAndAnalyze(expCtx,
                                                              findCommand->getProjection(),
                                                              out->filter.get(),
                                                              findCommand->getFilter(),
                                                              projectionPolicies));

            // Fail if any of the projection's dependencies are unavailable.
            DepsTracker{out->unavailableMetadata}.requestMetadata(out->proj->metadataDeps());
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    // If there is a sort, parse it and add any metadata dependencies it induces.
    if (!findCommand->getSort().isEmpty()) {
        try {
            out->sort.emplace(initSortPattern(findCommand, expCtx));
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    out->findCommandRequest = std::move(findCommand);
    return {std::move(out)};
}

}  // namespace

StatusWith<QueryMetadataBitSet> isValid(const MatchExpression* root,
                                        const FindCommandRequest& findCommand) {
    QueryMetadataBitSet unavailableMetadata{};

    // There can only be one TEXT.  If there is a TEXT, it cannot appear inside a NOR.
    //
    // Note that the query grammar (as enforced by the MatchExpression parser) forbids TEXT
    // inside of value-expression clauses like NOT, so we don't check those here.
    size_t numText = QueryPlannerCommon::countNodes(root, MatchExpression::TEXT);
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
    size_t numGeoNear = QueryPlannerCommon::countNodes(root, MatchExpression::GEO_NEAR);
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

StatusWith<std::pair<boost::intrusive_ptr<ExpressionContext>, std::unique_ptr<ParsedFindCommand>>>
parse(OperationContext* opCtx,
      std::unique_ptr<FindCommandRequest> findCommand,
      const ExtensionsCallback& extensionsCallback,
      MatchExpressionParser::AllowedFeatureSet allowedFeatures,
      const ProjectionPolicies& projectionPolicies) {
    // Make the expCtx.
    invariant(findCommand->getNamespaceOrUUID().nss());
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, *findCommand, resolveCollator(opCtx, findCommand), true /* mayDbProfile */);
    auto swResult = parseWithValidatedCollator(
        expCtx, std::move(findCommand), extensionsCallback, allowedFeatures, projectionPolicies);
    if (!swResult.isOK()) {
        return swResult.getStatus();
    }

    return std::pair{std::move(expCtx), std::move(swResult.getValue())};
}

StatusWith<std::unique_ptr<ParsedFindCommand>> parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<FindCommandRequest> findCommand,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    const ProjectionPolicies& projectionPolicies) {
    // A collator can enter through both the FindCommandRequest and ExpressionContext arguments.
    // This invariant ensures that both collators are the same because downstream we
    // pull the collator from only one of the ExpressionContext carrier.
    auto collator = resolveCollator(expCtx->opCtx, findCommand);
    if (collator.get() && expCtx->getCollator()) {
        invariant(CollatorInterface::collatorsMatch(collator.get(), expCtx->getCollator()));
    }
    return parseWithValidatedCollator(
        expCtx, std::move(findCommand), extensionsCallback, allowedFeatures, projectionPolicies);
}
}  // namespace mongo::parsed_find_command
