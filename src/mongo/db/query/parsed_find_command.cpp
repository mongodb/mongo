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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

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

/**
 * Create a CollatorInterface for both count and find commands.
 */
std::unique_ptr<CollatorInterface> resolveCollator(OperationContext* opCtx,
                                                   const BSONObj& collation) {
    if (collation.isEmpty()) {
        return nullptr;
    }
    return uassertStatusOKWithContext(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation),
        "unable to parse collation");
}
/**
 * Helper for building 'out.' If there is a projection, parse it and add any metadata dependencies
 * it induces.
 *
 * Throws exceptions if there is an error parsing the projection.
 */
void setProjection(ParsedFindCommand* out,
                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   const std::unique_ptr<FindCommandRequest>& findCommand,
                   const ProjectionPolicies& policies) {
    if (!findCommand->getProjection().isEmpty()) {
        out->savedProjectionPolicies.emplace(policies);
        out->proj.emplace(projection_ast::parseAndAnalyze(expCtx,
                                                          findCommand->getProjection(),
                                                          out->filter.get(),
                                                          findCommand->getFilter(),
                                                          policies));

        // This will throw if any of the projection's dependencies are unavailable.
        DepsTracker{out->availableMetadata}.setNeedsMetadata(out->proj->metadataDeps());
    }
}

/**
 * Helper for building 'out.' If there is a sort, parse it and add any metadata dependencies it
 * induces.
 *
 * Throws exceptions if there is an error parsing the sort pattern.
 */
void setSort(ParsedFindCommand* out,
             const boost::intrusive_ptr<ExpressionContext>& expCtx,
             const std::unique_ptr<FindCommandRequest>& findCommand) {
    if (!findCommand->getSort().isEmpty()) {
        // A $natural sort is really a hint, and should be handled as such. Furthermore, the
        // downstream sort handling code may not expect a $natural sort.
        //
        // We have already validated that if there is a $natural sort and a hint, that the hint
        // also specifies $natural with the same direction. Therefore, it is safe to clear the
        // $natural sort and rewrite it as a $natural hint.
        if (findCommand->getSort()[query_request_helper::kNaturalSortField]) {
            findCommand->setHint(findCommand->getSort().getOwned());
            findCommand->setSort(BSONObj{});
        }
        out->sort.emplace(findCommand->getSort(), expCtx);
    }
}

/**
 * Helper for building 'out.' If there is a sort, parse it and add any metadata dependencies it
 * induces.
 */
Status setSortAndProjection(ParsedFindCommand* out,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            const std::unique_ptr<FindCommandRequest>& findCommand,
                            const ProjectionPolicies& policies) {
    try {
        setProjection(out, expCtx, findCommand, policies);
        setSort(out, expCtx, findCommand);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

/**
 * Helper for building 'out.' Sets 'out->filter' and validates that it is well formed. In the
 * process, also populates 'out->availableMetadata.'
 */
Status setFilter(ParsedFindCommand* out,
                 std::unique_ptr<MatchExpression> filter,
                 const std::unique_ptr<FindCommandRequest>& findCommand) {
    // Verify the filter follows certain rules like there must be at most one text clause.
    auto swMeta = parsed_find_command::validateAndGetAvailableMetadata(filter.get(), *findCommand);
    if (!swMeta.isOK()) {
        return swMeta.getStatus();
    }
    out->availableMetadata = swMeta.getValue();
    out->filter = std::move(filter);
    return Status::OK();
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

    // Stop counting expressions after they have been parsed to exclude expressions created
    // during optimization and other processing steps.
    expCtx->stopExpressionCounters();

    if (auto status = setFilter(out.get(), std::move(statusWithMatcher.getValue()), findCommand);
        !status.isOK()) {
        return status;
    }

    if (auto status = setSortAndProjection(out.get(), expCtx, findCommand, projectionPolicies);
        !status.isOK()) {
        return status;
    }

    out->findCommandRequest = std::move(findCommand);
    return {std::move(out)};
}

}  // namespace

StatusWith<std::unique_ptr<ParsedFindCommand>> ParsedFindCommand::withExistingFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<CollatorInterface> collator,
    std::unique_ptr<MatchExpression> filter,
    std::unique_ptr<FindCommandRequest> findCommandRequest,
    const ProjectionPolicies& projectionPolicies) {
    auto out = std::make_unique<ParsedFindCommand>();
    out->collator = std::move(collator);
    if (auto status = setFilter(out.get(), std::move(filter), findCommandRequest); !status.isOK()) {
        return status;
    }
    if (auto status =
            setSortAndProjection(out.get(), expCtx, findCommandRequest, projectionPolicies);
        !status.isOK()) {
        return status;
    }
    out->findCommandRequest = std::move(findCommandRequest);
    return std::move(out);
}

namespace parsed_find_command {
StatusWith<QueryMetadataBitSet> validateAndGetAvailableMetadata(
    const MatchExpression* root, const FindCommandRequest& findCommand) {
    QueryMetadataBitSet availableMetadata{};

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
        availableMetadata.set(DocumentMetadataFields::kTextScore);
        availableMetadata.set(DocumentMetadataFields::kScore);
    }

    // There can only be one NEAR.  If there is a NEAR, it must be either the root or the root
    // must be an AND and its child must be a NEAR.
    size_t numGeoNear = QueryPlannerCommon::countNodes(root, MatchExpression::GEO_NEAR);
    if (numGeoNear > 1) {
        return Status(ErrorCodes::BadValue, "Too many geoNear expressions");
    } else if (1 == numGeoNear) {
        // Geo distance and geo point metadata are available.
        availableMetadata |= DepsTracker::kAllGeoNearData;
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

    return availableMetadata;
}

StatusWith<std::unique_ptr<ParsedFindCommand>> parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, ParsedFindCommandParams&& params) {
    // A collator can enter through both the FindCommandRequest and ExpressionContext arguments.
    // This invariant ensures that both collators are the same because downstream we
    // pull the collator from only one of the ExpressionContext carrier.
    auto collator =
        resolveCollator(expCtx->getOperationContext(), params.findCommand->getCollation());
    if (collator.get() && expCtx->getCollator()) {
        invariant(CollatorInterface::collatorsMatch(collator.get(), expCtx->getCollator()));
    }
    return parseWithValidatedCollator(expCtx,
                                      std::move(params.findCommand),
                                      params.extensionsCallback,
                                      params.allowedFeatures,
                                      params.projectionPolicies);
}

StatusWith<std::unique_ptr<ParsedFindCommand>> parseFromCount(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CountCommandRequest& countCommand,
    const ExtensionsCallback& extensionsCallback,
    const NamespaceString& nss) {
    auto collator = resolveCollator(expCtx->getOperationContext(),
                                    countCommand.getCollation().get_value_or({}));
    if (collator.get() && expCtx->getCollator()) {
        invariant(CollatorInterface::collatorsMatch(collator.get(), expCtx->getCollator()));
    }

    // Copy necessary count command fields to find command. Notably, the skip and limit fields are
    // _not_ copied because the count stage in the PlanExecutor already applies the skip and limit,
    // and thus copying the fields would involve erroneously skipping and limiting twice.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(countCommand.getQuery());
    findCommand->setCollation(countCommand.getCollation().value_or(BSONObj()));
    findCommand->setHint(countCommand.getHint());

    return parseWithValidatedCollator(
        expCtx,
        std::move(findCommand),
        extensionsCallback,
        // Currently, these are the only values used to parse a find command from a count command.
        // The projection policy is copied from ParsedFindCommandsParams. Future work could extend
        // this function by adding parameters for these values below.
        MatchExpressionParser::kAllowAllSpecialFeatures,
        ProjectionPolicies::findProjectionPolicies());
}
}  // namespace parsed_find_command
}  // namespace mongo
