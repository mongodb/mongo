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

#include "mongo/base/exact_cast.h"
#include "mongo/db/exec/document_value/document.h"

namespace mongo {
namespace projection_ast {
namespace {

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

void addNodeAtPath(ProjectionPathASTNode* root,
                   const FieldPath& path,
                   std::unique_ptr<ASTNode> newChild) {
    addNodeAtPathHelper(root, path, 0, std::move(newChild));
}

/**
 * Return a pair {begin, end} indicating where the first positional operator in the string is.
 * If there is no positional operator, returns boost::none.
 */
using PositionalProjectionLocation = boost::optional<std::pair<size_t, size_t>>;
PositionalProjectionLocation findFirstPositionalOperator(StringData fullPath) {
    size_t first = fullPath.find(".$.");
    if (first != std::string::npos) {
        return {{first, first + 3}};
    }

    // There are no cases of the positional operator in between paths, so it must be at the end.
    if (fullPath.endsWith(".$")) {
        return {{fullPath.size() - 2, fullPath.size()}};
    }

    // If the entire path is just a '$' we consider that part of the positional projection.
    // This case may arise if there are two positional projections in a row, such as "a.$.$".
    // For now such cases are banned elsewhere in the parsing code.
    if (fullPath == "$") {
        return {{0, fullPath.size()}};
    }

    return boost::none;
}

bool hasPositionalOperatorMatch(const MatchExpression* const query, StringData matchField) {
    if (query->getCategory() == MatchExpression::MatchCategory::kLogical) {
        for (unsigned int i = 0; i < query->numChildren(); ++i) {
            if (hasPositionalOperatorMatch(query->getChild(i), matchField)) {
                return true;
            }
        }
    } else {
        StringData queryPath = query->path();
        // We have to make a distinction between match expressions that are
        // initialized with an empty field/path name "" and match expressions
        // for which the path is not meaningful (eg. $where).
        if (!queryPath.rawData()) {
            return false;
        }
        StringData pathPrefix = str::before(queryPath, '.');
        return pathPrefix == matchField;
    }
    return false;
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
    boost::optional<ProjectType> type;

    // Whether there's an {_id: 1} projection.
    bool idIncludedEntirely = false;
};

void attemptToParseFindSlice(ParseContext* parseCtx,
                             const FieldPath& path,
                             const BSONObj& subObj,
                             ProjectionPathASTNode* parent) {
    if (subObj.firstElement().isNumber()) {
        addNodeAtPath(parent,
                      path,
                      std::make_unique<ProjectionSliceASTNode>(boost::none,
                                                               subObj.firstElement().numberInt()));
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

        uassert(31259,
                str::stream() << "$slice limit must be positive, got " << limitElt.numberInt(),
                limitElt.numberInt() > 0);
        addNodeAtPath(
            parent,
            path,
            std::make_unique<ProjectionSliceASTNode>(skipElt.numberInt(), limitElt.numberInt()));
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
    const bool isMeta = subObj.firstElementFieldNameStringData() == "$meta";
    uassert(31252,
            "Cannot use expression other than $meta in exclusion projection",
            !parseCtx->type || *parseCtx->type == ProjectType::kInclusion || isMeta);

    if (!isMeta) {
        parseCtx->type = ProjectType::kInclusion;
    }

    auto expr = Expression::parseExpression(
        parseCtx->expCtx, subObj, parseCtx->expCtx->variablesParseState);
    addNodeAtPath(parent, path, std::make_unique<ExpressionASTNode>(expr));
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
    }

    return attemptToParseGenericExpression(parseCtx, path, subObj, parent);
}

/**
 * Treats the given element as an inclusion projection, and update the tree as necessary.
 */
void parseInclusion(ParseContext* ctx,
                    BSONElement elem,
                    ProjectionPathASTNode* parent,
                    boost::optional<FieldPath> fullPathToParent,
                    PositionalProjectionLocation firstPositionalProjection) {
    // There are special rules about _id being included. _id may be included in both inclusion and
    // exclusion projections.
    const bool isTopLevelIdProjection = elem.fieldNameStringData() == "_id" && parent->isRoot();

    if (!firstPositionalProjection) {
        FieldPath path(elem.fieldNameStringData());
        addNodeAtPath(parent, path, std::make_unique<BooleanConstantASTNode>(true));

        if (isTopLevelIdProjection) {
            ctx->idIncludedEntirely = true;
        }
    } else {
        uassert(31276,
                "Cannot specify more than one positional projection per query.",
                !ctx->hasPositional);

        uassert(31256, "Cannot specify positional operator and $elemMatch.", !ctx->hasElemMatch);

        StringData matchField = fullPathToParent ? fullPathToParent->front()
                                                 : str::before(elem.fieldNameStringData(), '.');
        uassert(51050, "Projections with a positional operator require a matcher", ctx->query);
        uassert(31277,
                str::stream() << "Positional projection '" << elem.fieldName() << "' does not "
                              << "match the query document.",
                hasPositionalOperatorMatch(ctx->query, matchField));

        // Check that the path does not end with ".$." which can be interpreted as the
        // positional projection.
        uassert(31270,
                str::stream() << "Path cannot end with '.$.'",
                !elem.fieldNameStringData().endsWith(".$."));

        const auto [firstPositionalBegin, firstPositionalEnd] = *firstPositionalProjection;

        // See if there's another positional operator after the first one. If there is,
        // it's invalid.
        StringData remainingPathAfterPositional =
            elem.fieldNameStringData().substr(firstPositionalEnd);
        uassert(31287,
                str::stream() << "Cannot use positional operator twice: "
                              << elem.fieldNameStringData(),
                findFirstPositionalOperator(remainingPathAfterPositional) == boost::none &&
                    remainingPathAfterPositional != "$");

        // Get everything up to the first positional operator.
        StringData pathWithoutPositionalOperator =
            elem.fieldNameStringData().substr(0, firstPositionalBegin);

        FieldPath path(pathWithoutPositionalOperator);

        auto matcher = CopyableMatchExpression{ctx->queryObj,
                                               ctx->expCtx,
                                               std::make_unique<ExtensionsCallbackNoop>(),
                                               MatchExpressionParser::kBanAllSpecialFeatures,
                                               true /* optimize expression */};

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
    }
}

/**
 * Treats the given element as a literal value (e.g. {a: "foo"}) and updates the tree as necessary.
 */
void parseLiteral(ParseContext* ctx, BSONElement elem, ProjectionPathASTNode* parent) {
    auto expr = Expression::parseOperand(ctx->expCtx, elem, ctx->expCtx->variablesParseState);

    FieldPath pathFromParent(elem.fieldNameStringData());
    addNodeAtPath(parent, pathFromParent, std::make_unique<ExpressionASTNode>(expr));

    uassert(31310,
            str::stream() << "Cannot use an expression " << elem << " in an exclusion projection",
            !ctx->type || *ctx->type == ProjectType::kInclusion);
    ctx->type = ProjectType::kInclusion;
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
    FieldPath path(objFieldName);

    if (obj.nFields() == 1 && obj.firstElementFieldNameStringData().startsWith("$")) {
        // Maybe it's an expression.
        const bool isExpression = parseSubObjectAsExpression(ctx, path, obj, parent);

        if (isExpression) {
            return;
        }

        // It was likely intended to be an expression. Check if it's a valid field path or not to
        // confirm.
        try {
            FieldPath fp(obj.firstElementFieldNameStringData());
        } catch (const DBException&) {
            uasserted(31325,
                      str::stream()
                          << "Unknown expression " << obj.firstElementFieldNameStringData());
        }
    }

    // It's not an expression. Create a node to represent the new layer in the tree.
    ProjectionPathASTNode* newParent = nullptr;
    {
        auto ownedChild = std::make_unique<ProjectionPathASTNode>();
        newParent = ownedChild.get();
        parent->addChild(objFieldName, std::move(ownedChild));
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
    const auto firstPositionalProjection = findFirstPositionalOperator(elem.fieldNameStringData());

    // If there is a positional projection, find only features must be enabled.
    uassert(31324,
            "Cannot use positional projection in aggregation projection",
            (!firstPositionalProjection || ctx->policies.findOnlyFeaturesAllowed()));

    if (elem.type() == BSONType::Object) {
        BSONObj subObj = elem.embeddedObject();

        // Make sure this isn't a positional operator. It's illegal to combine positional with
        // any expression.
        uassert(31271,
                "positional projection cannot be used with an expression or sub object",
                static_cast<bool>(!firstPositionalProjection));

        parseSubObject(ctx, elem.fieldNameStringData(), fullPathToParent, subObj, parent);
    } else if (isInclusionOrExclusionType(elem.type())) {
        if (elem.trueValue()) {
            parseInclusion(ctx, elem, parent, fullPathToParent, firstPositionalProjection);
        } else {
            parseExclusion(ctx, elem, parent);
        }
    } else {
        uassert(31308,
                "positional projection cannot be used with a literal",
                static_cast<bool>(!firstPositionalProjection));

        parseLiteral(ctx, elem, parent);
    }
}
}  // namespace

Projection parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                 const BSONObj& obj,
                 const MatchExpression* const query,
                 const BSONObj& queryObj,
                 ProjectionPolicies policies) {
    ProjectionPathASTNode root;

    ParseContext ctx{expCtx, query, queryObj, obj, policies};

    for (auto&& elem : obj) {
        ctx.idSpecified |=
            elem.fieldNameStringData() == "_id" || elem.fieldNameStringData().startsWith("_id.");
        parseElement(&ctx, elem, boost::none, &root);
    }

    // find() defaults about inclusion/exclusion. These rules are preserved for compatibility
    // reasons. If there are no explicit inclusion/exclusion fields, the type depends on which
    // find() expressions (if any) are used in the following order: $slice, $elemMatch, $meta.
    if (!ctx.type) {
        if (ctx.idIncludedEntirely) {
            // The projection {_id: 1} is considered an inclusion. The ParseContext's type field was
            // not marked as such, because a projection {_id: 1, a: 0} is also valid, but considered
            // exclusion.
            ctx.type = ProjectType::kInclusion;
        } else if (ctx.hasFindSlice) {
            // If the projection has only find() expressions, then $slice has highest precedence.
            ctx.type = ProjectType::kExclusion;
        } else if (ctx.hasElemMatch) {
            // $elemMatch has next-highest precedent.
            ctx.type = ProjectType::kInclusion;
        } else {
            // This happens only when the projection is entirely $meta expressions.
            ctx.type = ProjectType::kExclusion;
        }
    }
    invariant(ctx.type);

    if (!ctx.idSpecified && policies.idPolicy == ProjectionPolicies::DefaultIdPolicy::kIncludeId &&
        *ctx.type == ProjectType::kInclusion) {
        // Add a node to the root indicating that _id is included.
        addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(true));
    }

    if (*ctx.type == ProjectType::kExclusion && ctx.idSpecified && ctx.idIncludedEntirely) {
        // The user explicitly included _id in an exclusion projection. This is legal syntax, but
        // the node indicating that _id is included doesn't need to be in the tree.
        invariant(root.removeChild("_id"));
    }

    return Projection{std::move(root), *ctx.type};
}

Projection parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                 const BSONObj& obj,
                 ProjectionPolicies policies) {
    return parse(std::move(expCtx), obj, nullptr, BSONObj(), std::move(policies));
}
}  // namespace projection_ast
}  // namespace mongo
