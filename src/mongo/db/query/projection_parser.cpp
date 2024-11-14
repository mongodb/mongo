/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/query/projection_parser.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/exact_cast.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/copyable_match_expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

namespace mongo {
namespace projection_ast {
namespace {
/**
 * Uassert that the given policy permits using computed fields in a projection.
 */
void verifyComputedFieldsAllowed(const ProjectionPolicies& policies) {
    uassert(51271,
            "Bad projection specification, cannot use computed fields when parsing "
            "a spec in kBanComputedFields mode",
            policies.computedFieldsPolicy !=
                ProjectionPolicies::ComputedFieldsPolicy::kBanComputedFields);
}

/**
 * In some arcane situations, when a projection is empty, only contains top-level _id projections
 * and find expressions, it is non-trivial to determine the type of the projection. These rules are
 * kept purely for compatibility reasons.
 *
 * The significance of an _id inclusion or exclusion depends on the presence of the expressions find
 * $slice, $elemMatch and $meta.
 */
ProjectType computeProjectionType(bool hasFindSlice,
                                  bool hasElemMatch,
                                  bool hasMeta,
                                  const ProjectionPolicies& policies,
                                  bool idIncludedEntirely,
                                  bool idExcludedEntirely) {
    if (hasFindSlice) {
        // If there's a find $slice then the presence of an {_id: 1} overrides, regardless of other
        // find() expressions. If there's no _id, then it defaults to exclusion.
        if (idIncludedEntirely) {
            return ProjectType::kInclusion;
        } else {
            // Either _id is explicitly excluded _or_ not mentioned at all, in which case we
            // default to exclusion.
            return ProjectType::kExclusion;
        }
    } else if (hasElemMatch) {
        // If there's an $elemMatch (but no $slice) then it's an inclusion projection. Note that
        // this is _regardless_ of what value is provided for _id. This is consistent with the
        // behavior of most expressions: for an arbitrary $func expression, the rule is that {foo:
        // {$func: ...}}, {_id: 0, foo: {$func: ...}}, and {_id: 1, foo: {$func: ...}} are all
        // inclusions.
        return ProjectType::kInclusion;
    } else if (hasMeta) {
        if (policies.findOnlyFeaturesAllowed()) {
            // In find, {_id: 0, x: {$meta: ...}} is considered exclusion.
            if (idExcludedEntirely) {
                return ProjectType::kExclusion;
            }

            // In find, {_id: 1, x: {$meta: ...}} is considered inclusion.
            if (idIncludedEntirely) {
                return ProjectType::kInclusion;
            }

            // Just $meta by itself is exclusion.
            return ProjectType::kExclusion;
        } else {
            // In aggregate(), any projection with a $meta is an inclusion projection.
            return ProjectType::kInclusion;
        }
    } else if (idIncludedEntirely) {
        // There were no expressions. So this is an {_id: 1} projection. It is an
        // inclusion. The ParseContext's type field was not marked as an inclusion, because a
        // projection {_id: 1, a: 0} is also valid, but considered exclusion.
        return ProjectType::kInclusion;
    } else if (idExcludedEntirely) {
        // There were no expressions, but there is an {_id: 0} element. This is an exclusion.  The
        // ParseContext's 'type' field was not marked as an exclusion because a projection {_id: 0,
        // a: 1} is valid but considered inclusion.
        return ProjectType::kExclusion;
    }

    // Default is exclusion otherwise.
    return ProjectType::kExclusion;
}

/**
 * Returns whether an element's type implies that the element is an inclusion or exclusion.
 */
bool isInclusionOrExclusionType(BSONType type) {
    switch (type) {
        case BSONType::Bool:
        case BSONType::NumberInt:
        case BSONType::NumberLong:
        case BSONType::NumberDouble:
        case BSONType::NumberDecimal:
            return true;
        default:
            return false;
    }
}

/**
 * Given the 'root' of the AST and the field 'path', returns the last inner 'ProjectionPathASTNode'
 * in the AST on that 'path'. For example, if the AST represents a projection {'a.b.c': 1} and the
 * 'path' is 'a.b',  the returned node will be 'b'. If the node doesn't exist in the tree, or if the
 * last node is a leaf node, the function returns 'nullptr'. For example, given the same projection
 * specification and the 'path' of 'a.b.c.d', the function will return 'nullptr'.
 */
ProjectionPathASTNode* findLastInnerNodeOnPath(ProjectionPathASTNode* root,
                                               const FieldPath& path,
                                               size_t componentIndex) {
    invariant(root);
    invariant(path.getPathLength() > componentIndex);

    auto child = exact_pointer_cast<ProjectionPathASTNode*>(
        root->getChild(path.getFieldName(componentIndex)));
    if (path.getPathLength() == componentIndex + 1) {
        return child;
    } else if (!child) {
        return nullptr;
    }

    return findLastInnerNodeOnPath(child, path, componentIndex + 1);
}

void addNodeAtPathHelper(ProjectionPathASTNode* root,
                         const FieldPath& path,
                         size_t componentIndex,
                         std::unique_ptr<ASTNode> newChild) {
    invariant(root);
    invariant(path.getPathLength() > componentIndex);
    const auto nextComponent = path.getFieldName(componentIndex);

    ASTNode* child = root->getChild(nextComponent);

    if (path.getPathLength() == componentIndex + 1) {
        uassert(31250, str::stream() << "Path collision at " << path.fullPath(), !child);

        root->addChild(nextComponent.toString(), std::move(newChild));
        return;
    }

    if (!child) {
        auto newInternalChild = std::make_unique<ProjectionPathASTNode>();
        auto rawInternalChild = newInternalChild.get();
        root->addChild(nextComponent.toString(), std::move(newInternalChild));
        addNodeAtPathHelper(rawInternalChild, path, componentIndex + 1, std::move(newChild));
        return;
    }

    // Either find or create an internal node.
    auto* childPathNode = exact_pointer_cast<ProjectionPathASTNode*>(child);
    uassert(31249,
            str::stream() << "Path collision at " << path.fullPath() << " remaining portion "
                          << path.tail().fullPath(),
            childPathNode != nullptr);

    addNodeAtPathHelper(childPathNode, path, componentIndex + 1, std::move(newChild));
}

bool hasPositionalOperator(StringData path) {
    return path.endsWith(".$");
}

bool isPrefixOf(StringData first, StringData second) {
    if (first.size() >= second.size()) {
        return false;
    }

    return second.startsWith(first) && second[first.size()] == '.';
}

struct ParseContext {
    const boost::intrusive_ptr<ExpressionContext> expCtx;

    // Properties of the projection/query.
    const MatchExpression* const query = nullptr;
    const BSONObj& queryObj;

    const BSONObj& spec;
    const ProjectionPolicies policies;

    // Properties of the projection that need to be stored for later checks.
    bool idSpecified = false;
    bool hasPositional = false;
    bool hasElemMatch = false;
    bool hasFindSlice = false;
    bool hasMeta = false;
    boost::optional<ProjectType> type;

    // Whether there's an {_id: 1} field in the projection.
    bool idIncludedEntirely = false;

    // Whether there's an {_id: 0} field in the projection.
    bool idExcludedEntirely = false;
};

void attemptToParseFindSlice(ParseContext* parseCtx,
                             const FieldPath& path,
                             const BSONObj& subObj,
                             ProjectionPathASTNode* parent) {
    if (subObj.firstElement().isNumber()) {
        addNodeAtPath(parent,
                      path,
                      std::make_unique<ProjectionSliceASTNode>(
                          boost::none, subObj.firstElement().safeNumberInt()));
    } else if (subObj.firstElementType() == BSONType::Array) {
        BSONObj arr = subObj.firstElement().embeddedObject();
        uassert(31272, "$slice array argument should be of form [skip, limit]", arr.nFields() == 2);

        BSONObjIterator it(arr);
        BSONElement skipElt = it.next();
        BSONElement limitElt = it.next();
        uassert(31257,
                str::stream() << "$slice expects the skip argument to be a number, got "
                              << skipElt.type(),
                skipElt.isNumber());
        uassert(31258,
                str::stream() << "$slice expects the limit argument to be a number, got "
                              << limitElt.type(),
                limitElt.isNumber());

        auto limit = limitElt.safeNumberInt();
        uassert(31259, str::stream() << "$slice limit must be positive, got " << limit, limit > 0);
        addNodeAtPath(
            parent, path, std::make_unique<ProjectionSliceASTNode>(skipElt.safeNumberInt(), limit));
    } else {
        uasserted(31273, "$slice only supports numbers and [skip, limit] arrays");
    }

    parseCtx->hasFindSlice = true;
}

/**
 * If the object matches the form of an expression ({<valid expression name>: <arguments>}) then
 * attempts to parse it as an expression and add it to the tree.

 * Returns whether the object matched the form an actual expression.
 */
bool attemptToParseGenericExpression(ParseContext* parseCtx,
                                     const FieldPath& path,
                                     const BSONObj& subObj,
                                     ProjectionPathASTNode* parent) {
    if (!Expression::isExpressionName(subObj.firstElementFieldNameStringData())) {
        return false;
    }

    // It must be an expression.
    verifyComputedFieldsAllowed(parseCtx->policies);

    const bool isMeta = subObj.firstElementFieldNameStringData() == "$meta";
    uassert(31252,
            "Cannot use expression other than $meta in exclusion projection",
            !parseCtx->type || *parseCtx->type != ProjectType::kExclusion || isMeta);

    if (!isMeta && !parseCtx->type) {
        parseCtx->type = ProjectType::kInclusion;
    }

    auto expr = Expression::parseExpression(
        parseCtx->expCtx.get(), subObj, parseCtx->expCtx->variablesParseState);
    addNodeAtPath(parent, path, std::make_unique<ExpressionASTNode>(expr));
    parseCtx->hasMeta = parseCtx->hasMeta || isMeta;
    return true;
}


/**
 * Treats the given object as either a find()-only expression ($slice, $elemMatch) if allowed,
 * or as a generic agg expression, and adds it to the the tree.
 *
 * Returns whether or not the object matched the form of an expression. If not,
 * the sub object must represent a sub-projection (e.g. {a: {b: 1}}).
 */
bool parseSubObjectAsExpression(ParseContext* parseCtx,
                                const FieldPath& path,
                                const BSONObj& subObj,
                                ProjectionPathASTNode* parent) {
    if (parseCtx->policies.findOnlyFeaturesAllowed()) {
        if (subObj.firstElementFieldNameStringData() == "$slice") {
            verifyComputedFieldsAllowed(parseCtx->policies);

            Status findSliceStatus = Status::OK();
            try {
                attemptToParseFindSlice(parseCtx, path, subObj, parent);
                return true;
            } catch (const DBException& exn) {
                findSliceStatus = exn.toStatus();
            }

            try {
                attemptToParseGenericExpression(parseCtx, path, subObj, parent);
            } catch (DBException& exn) {
                exn.addContext(str::stream()
                               << "Invalid $slice syntax. The given syntax " << subObj
                               << " did not match the find() syntax because :: " << findSliceStatus
                               << " :: "
                               << "The given syntax did not match the expression $slice syntax.");
                throw;
            }

            return true;
        } else if (subObj.firstElementFieldNameStringData() == "$elemMatch") {
            verifyComputedFieldsAllowed(parseCtx->policies);

            // Validate $elemMatch arguments and dependencies.
            uassert(31274,
                    str::stream() << "elemMatch: Invalid argument, object required, but got "
                                  << subObj.firstElementType(),
                    subObj.firstElementType() == BSONType::Object);

            uassert(31255,
                    "Cannot specify positional operator and $elemMatch.",
                    !parseCtx->hasPositional);

            uassert(31275,
                    "Cannot use $elemMatch projection on a nested field.",
                    path.getPathLength() == 1 && parent->isRoot());

            // Create a MatchExpression for the elemMatch.
            BSONObj elemMatchObj = BSON(path.fullPath() << subObj);
            invariant(elemMatchObj.isOwned());

            auto matcher = CopyableMatchExpression{elemMatchObj,
                                                   parseCtx->expCtx,
                                                   std::make_unique<ExtensionsCallbackNoop>(),
                                                   MatchExpressionParser::kBanAllSpecialFeatures,
                                                   true /* optimize expression */};
            auto matchNode = std::make_unique<MatchExpressionASTNode>(std::move(matcher));

            addNodeAtPath(
                parent, path, std::make_unique<ProjectionElemMatchASTNode>(std::move(matchNode)));
            parseCtx->hasElemMatch = true;
            return true;
        }
    } else if (subObj.firstElementFieldNameStringData() == "$elemMatch") {
        // find()-only features are not and the user tried invoking elemMatch. Here we can give a
        // nicer error than the generic "unknown expression."
        uasserted(ErrorCodes::InvalidPipelineOperator, "Cannot use $elemMatch in this context");
    }

    return attemptToParseGenericExpression(parseCtx, path, subObj, parent);
}

/**
 * Treats the given element as an inclusion projection, and update the tree as necessary.
 */
void parseInclusion(ParseContext* ctx,
                    BSONElement elem,
                    ProjectionPathASTNode* parent,
                    boost::optional<FieldPath> fullPathToParent) {
    // There are special rules about _id being included. _id may be included in both inclusion and
    // exclusion projections.
    const bool isTopLevelIdProjection = elem.fieldNameStringData() == "_id" && parent->isRoot();

    const bool hasPositional = hasPositionalOperator(elem.fieldNameStringData());

    if (!hasPositional) {
        FieldPath path(elem.fieldNameStringData());
        addNodeAtPath(parent, path, std::make_unique<BooleanConstantASTNode>(true));

        if (isTopLevelIdProjection) {
            ctx->idIncludedEntirely = true;
        }
    } else {
        verifyComputedFieldsAllowed(ctx->policies);
        StringData elemFieldName = elem.fieldNameStringData();

        uassert(31276,
                "Cannot specify more than one positional projection per query.",
                !ctx->hasPositional);

        uassert(31256, "Cannot specify positional operator and $elemMatch.", !ctx->hasElemMatch);
        uassert(51050, "Projections with a positional operator require a matcher", ctx->query);

        // Special case: ".$" is not considered a valid projection.
        uassert(5392900,
                str::stream() << "Projection on field " << elemFieldName << " is invalid",
                elemFieldName != ".$");

        // Get everything up to the first positional operator.
        tassert(5392901,
                "Expected element field name size to be greater than 2",
                elemFieldName.size() > 2);
        StringData pathWithoutPositionalOperator =
            elemFieldName.substr(0, elemFieldName.size() - 2);

        FieldPath path(pathWithoutPositionalOperator);

        // Parse the match expression in order to ensure that no special features are used with
        // positional projection.
        uassertStatusOK(
            MatchExpressionParser::parse(ctx->queryObj,
                                         ctx->expCtx,
                                         ExtensionsCallbackNoop{},
                                         MatchExpressionParser::kBanAllSpecialFeatures));

        // Copy the original match expression, which makes sure to preserve any input parameter ids
        // attached to the tree.
        CopyableMatchExpression matcher{ctx->queryObj, ctx->query->clone()};

        invariant(ctx->query);
        addNodeAtPath(parent,
                      path,
                      std::make_unique<ProjectionPositionalASTNode>(
                          std::make_unique<MatchExpressionASTNode>(matcher)));

        ctx->hasPositional = true;
    }

    if (!isTopLevelIdProjection) {
        uassert(31253,
                str::stream() << "Cannot do inclusion on field " << elem.fieldNameStringData()
                              << " in exclusion projection",
                !ctx->type || *ctx->type == ProjectType::kInclusion);
        ctx->type = ProjectType::kInclusion;
    }
}

/**
 * Treates the given element as an exclusion projection and updates the tree as necessary.
 */
void parseExclusion(ParseContext* ctx, BSONElement elem, ProjectionPathASTNode* parent) {
    invariant(!elem.trueValue());
    FieldPath path(elem.fieldNameStringData());
    addNodeAtPath(parent, path, std::make_unique<BooleanConstantASTNode>(false));

    const bool isTopLevelIdProjection = elem.fieldNameStringData() == "_id" && parent->isRoot();
    if (!isTopLevelIdProjection) {
        uassert(31254,
                str::stream() << "Cannot do exclusion on field " << elem.fieldNameStringData()
                              << " in inclusion projection",
                !ctx->type || *ctx->type == ProjectType::kExclusion);
        ctx->type = ProjectType::kExclusion;
    } else {
        ctx->idExcludedEntirely = true;
    }
}

/**
 * Treats the given element as a literal value (e.g. {a: "foo"}) and updates the tree as necessary.
 */
void parseLiteral(ParseContext* ctx, BSONElement elem, ProjectionPathASTNode* parent) {
    verifyComputedFieldsAllowed(ctx->policies);

    auto expr = Expression::parseOperand(ctx->expCtx.get(), elem, ctx->expCtx->variablesParseState);

    FieldPath pathFromParent(elem.fieldNameStringData());
    addNodeAtPath(parent, pathFromParent, std::make_unique<ExpressionASTNode>(expr));

    if (ctx->policies.computedFieldsPolicy !=
        ProjectionPolicies::ComputedFieldsPolicy::kOnlyComputedFields) {
        uassert(31310,
                str::stream() << "Cannot use an expression " << elem
                              << " in an exclusion projection",
                !ctx->type || *ctx->type == ProjectType::kInclusion);
        ctx->type = ProjectType::kInclusion;
    }
}

// Mutually recursive with parseSubObject().
void parseElement(ParseContext* ctx,
                  BSONElement elem,
                  boost::optional<FieldPath> fullPathToParent,
                  ProjectionPathASTNode* parent);

/**
 * Parses the given object and updates the tree as necessary.
 * This function will do the work of determining whether the sub object should be treated as an
 * expression or subprojection.
 */
void parseSubObject(ParseContext* ctx,
                    StringData objFieldName,
                    boost::optional<FieldPath> fullPathToParent,
                    const BSONObj& obj,
                    ProjectionPathASTNode* parent) {
    uassert(
        51270, str::stream() << "Invalid empty sub-projection: " << objFieldName, !obj.isEmpty());

    FieldPath path(objFieldName);

    if (obj.nFields() == 1 && obj.firstElementFieldNameStringData().starts_with('$')) {
        // Maybe it's an expression.
        const bool isExpression = parseSubObjectAsExpression(ctx, path, obj, parent);

        if (isExpression) {
            return;
        }

        // It was likely intended to be an expression. Check if it's a valid field path or not to
        // confirm.
        try {
            const auto elementFieldName = obj.firstElementFieldNameStringData();
            if (!hasPositionalOperator(elementFieldName)) {
                FieldPath fp(elementFieldName);
            } else {
                // The 'FieldPath' parser doesn't take positional operators into account, but those
                // are valid path projections so trim it off for this validation.
                StringData pathWithoutPositionalOperator =
                    elementFieldName.substr(0, elementFieldName.size() - 2);
                FieldPath fp(pathWithoutPositionalOperator);
            }
        } catch (const DBException&) {
            uasserted(31325,
                      str::stream()
                          << "Unknown expression " << obj.firstElementFieldNameStringData());
        }
    }

    ProjectionPathASTNode* newParent = findLastInnerNodeOnPath(parent, path, 0);
    if (!newParent) {
        auto ownedChild = std::make_unique<ProjectionPathASTNode>();
        newParent = ownedChild.get();
        addNodeAtPath(parent, path, std::move(ownedChild));
    }

    const FieldPath fullPathToNewParent = fullPathToParent ? fullPathToParent->concat(path) : path;
    for (auto&& elem : obj) {
        parseElement(ctx, elem, fullPathToNewParent, newParent);
    }
}

/**
 * Determine what "type" this element of the projection is (inclusion, exclusion, sub object,
 * literal) and update the tree accordingly.
 */
void parseElement(ParseContext* ctx,
                  BSONElement elem,
                  boost::optional<FieldPath> fullPathToParent,
                  ProjectionPathASTNode* parent) {
    const bool hasPositional = hasPositionalOperator(elem.fieldNameStringData());

    // If there is a positional projection, find only features must be enabled.
    uassert(31324,
            "Cannot use positional projection in aggregation projection",
            (!hasPositional || ctx->policies.findOnlyFeaturesAllowed()));

    // Be sure that uses of positional projection that were correct in versions before 4.4 that are
    // now incorrect get a good error message.
    uassert(31394,
            "As of 4.4, it's illegal to specify positional operator in the middle of a path."
            "Positional projection may only be used at the end, for example: a.b.$. If the query "
            "previously used a form like a.b.$.d, remove the parts following the '$' and the "
            "results will be equivalent.",
            !str::contains(elem.fieldNameStringData(), ".$."));

    if (elem.type() == BSONType::Object) {
        BSONObj subObj = elem.embeddedObject();
        // Uninitialized 'ctx->type' is default treated as ProjectType::kInclusion.
        if (!ctx->type || *ctx->type != ProjectType::kAddition || !subObj.isEmpty()) {
            // Make sure this isn't a positional operator. It's illegal to combine positional with
            // any expression.
            uassert(31271,
                    "positional projection cannot be used with an expression or sub object",
                    !hasPositional);

            parseSubObject(ctx, elem.fieldNameStringData(), fullPathToParent, subObj, parent);
        } else {
            // subObj.isEmpty() in the ProjectType::kAddition case. This occurs when a pipeline
            // stage like {$addFields: {myField: {}}} is pushed down to SBE and the current object
            // is the one holding "myField". In this case we want to treat {myField: {}} as a
            // projection literal, i.e. add "myField" with literal value {}.
            parseLiteral(ctx, elem, parent);
        }
    } else if (ctx->policies.computedFieldsPolicy !=
                   ProjectionPolicies::ComputedFieldsPolicy::kOnlyComputedFields &&
               isInclusionOrExclusionType(elem.type())) {
        if (elem.trueValue()) {
            parseInclusion(ctx, elem, parent, fullPathToParent);
        } else {
            uassert(31395, "positional projection cannot be used with exclusion", !hasPositional);
            parseExclusion(ctx, elem, parent);
        }
    } else {
        uassert(31308, "positional projection cannot be used with a literal", !hasPositional);

        parseLiteral(ctx, elem, parent);
    }
}
}  // namespace

Projection parseAndAnalyze(boost::intrusive_ptr<ExpressionContext> expCtx,
                           const BSONObj& obj,
                           const MatchExpression* const query,
                           const BSONObj& queryObj,
                           ProjectionPolicies policies,
                           bool shouldOptimize) {
    if (!policies.emptyProjectionAllowed()) {
        // In agg-style syntax it is illegal to have an empty projection specification.
        uassert(51272, "projection specification must have at least one field", !obj.isEmpty());
    }

    ProjectionPathASTNode root;

    ParseContext ctx{expCtx, query, queryObj, obj, policies};

    // $addFields is treated as a projection that has only computed fields.
    if (policies.computedFieldsPolicy ==
        ProjectionPolicies::ComputedFieldsPolicy::kOnlyComputedFields) {
        ctx.type = ProjectType::kAddition;
    }

    for (auto&& elem : obj) {
        if (elem.fieldNameStringData().starts_with("_")) {
            ctx.idSpecified |= elem.fieldNameStringData() == "_id" ||
                elem.fieldNameStringData().startsWith("_id.");
        }

        parseElement(&ctx, elem, boost::none, &root);
    }

    // If we have not yet determined the type, we must fall back to the defaults for ambiguous
    // projections.
    if (!ctx.type) {
        ctx.type = computeProjectionType(ctx.hasFindSlice,
                                         ctx.hasElemMatch,
                                         ctx.hasMeta,
                                         ctx.policies,
                                         ctx.idIncludedEntirely,
                                         ctx.idExcludedEntirely);
    }
    invariant(ctx.type);

    if (!ctx.idSpecified) {
        if (policies.idPolicy == ProjectionPolicies::DefaultIdPolicy::kIncludeId &&
            *ctx.type == ProjectType::kInclusion) {
            // Add a node to the root indicating that _id is included.
            addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(true));
        } else if (policies.idPolicy == ProjectionPolicies::DefaultIdPolicy::kExcludeId &&
                   *ctx.type == ProjectType::kExclusion) {
            // Add a node to the root indicating that _id is not included.
            addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(false));
        }
    }

    if (*ctx.type == ProjectType::kExclusion && ctx.idSpecified && ctx.idIncludedEntirely) {
        // The user explicitly included _id in an exclusion projection. This is legal syntax, but
        // the node indicating that _id is included doesn't need to be in the tree.
        invariant(root.removeChild("_id"));
    }

    // Optimize the projection expression if requested and as long as not explicitly disabled
    // pipeline optimization.
    auto fp = globalFailPointRegistry().find("disablePipelineOptimization");
    if (shouldOptimize && !(fp && fp->shouldFail())) {
        optimizeProjection(&root);
    }

    return Projection{std::move(root), *ctx.type};
}

Projection parseAndAnalyze(boost::intrusive_ptr<ExpressionContext> expCtx,
                           const BSONObj& obj,
                           ProjectionPolicies policies,
                           bool shouldOptimize) {
    return parseAndAnalyze(
        std::move(expCtx), obj, nullptr, BSONObj(), std::move(policies), shouldOptimize);
}

void addNodeAtPath(ProjectionPathASTNode* root,
                   const FieldPath& path,
                   std::unique_ptr<ASTNode> newChild) {
    addNodeAtPathHelper(root, path, 0, std::move(newChild));
}

}  // namespace projection_ast
}  // namespace mongo
