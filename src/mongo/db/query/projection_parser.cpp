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
boost::optional<std::pair<size_t, size_t>> findFirstPositionalOperator(StringData fullPath) {
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
}  // namespace

Projection parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                 const BSONObj& obj,
                 const MatchExpression* const query,
                 const BSONObj& queryObj,
                 ProjectionPolicies policies) {
    // TODO: SERVER-42988 Support agg syntax with nesting.

    ProjectionPathASTNode root;

    bool idSpecified = false;

    bool hasPositional = false;
    bool hasElemMatch = false;
    bool hasFindSlice = false;
    boost::optional<ProjectType> type;

    for (auto&& elem : obj) {
        idSpecified |=
            elem.fieldNameStringData() == "_id" || elem.fieldNameStringData().startsWith("_id.");

        const auto firstPositionalProjection =
            findFirstPositionalOperator(elem.fieldNameStringData());

        if (elem.type() == BSONType::Object) {
            BSONObj subObj = elem.embeddedObject();

            // Make sure this isn't a positional operator. It's illegal to combine positional with
            // any expression.
            uassert(31271,
                    "positional projection cannot be used with an expression",
                    !firstPositionalProjection);

            FieldPath path(elem.fieldNameStringData());

            if (subObj.firstElementFieldNameStringData() == "$slice") {
                if (subObj.firstElement().isNumber()) {
                    addNodeAtPath(&root,
                                  path,
                                  std::make_unique<ProjectionSliceASTNode>(
                                      boost::none, subObj.firstElement().numberInt()));
                } else if (subObj.firstElementType() == BSONType::Array) {
                    BSONObj arr = subObj.firstElement().embeddedObject();
                    uassert(31272,
                            "$slice array argument should be of form [skip, limit]",
                            arr.nFields() == 2);

                    BSONObjIterator it(arr);
                    BSONElement skipElt = it.next();
                    BSONElement limitElt = it.next();
                    uassert(31257,
                            str::stream() << "$slice expects the skip argument to be a number, got "
                                          << skipElt.type(),
                            skipElt.isNumber());
                    uassert(31258,
                            str::stream()
                                << "$slice expects the limit argument to be a number, got "
                                << limitElt.type(),
                            limitElt.isNumber());


                    uassert(31259,
                            str::stream()
                                << "$slice limit must be positive, got " << limitElt.numberInt(),
                            limitElt.numberInt() > 0);
                    addNodeAtPath(&root,
                                  path,
                                  std::make_unique<ProjectionSliceASTNode>(skipElt.numberInt(),
                                                                           limitElt.numberInt()));
                } else {
                    uasserted(31273, "$slice only supports numbers and [skip, limit] arrays");
                }

                hasFindSlice = true;
            } else if (subObj.firstElementFieldNameStringData() == "$elemMatch") {
                // Validate $elemMatch arguments and dependencies.
                uassert(31274,
                        str::stream() << "elemMatch: Invalid argument, object required, but got "
                                      << subObj.firstElementType(),
                        subObj.firstElementType() == BSONType::Object);

                uassert(
                    31255, "Cannot specify positional operator and $elemMatch.", !hasPositional);

                uassert(31275,
                        "Cannot use $elemMatch projection on a nested field.",
                        !str::contains(elem.fieldName(), '.'));

                // Create a MatchExpression for the elemMatch.
                BSONObj elemMatchObj = elem.wrap();
                invariant(elemMatchObj.isOwned());

                auto matcher =
                    CopyableMatchExpression{elemMatchObj,
                                            expCtx,
                                            std::make_unique<ExtensionsCallbackNoop>(),
                                            MatchExpressionParser::kBanAllSpecialFeatures,
                                            true /* optimize expression */};
                auto matchNode = std::make_unique<MatchExpressionASTNode>(std::move(matcher));

                addNodeAtPath(&root,
                              path,
                              std::make_unique<ProjectionElemMatchASTNode>(std::move(matchNode)));
                hasElemMatch = true;
            } else {
                const bool isMeta = subObj.firstElementFieldNameStringData() == "$meta";

                uassert(31252,
                        "Cannot use expression other than $meta in exclusion projection",
                        !type || *type == ProjectType::kInclusion || isMeta);

                if (!isMeta) {
                    type = ProjectType::kInclusion;
                }

                auto expr =
                    Expression::parseExpression(expCtx, subObj, expCtx->variablesParseState);
                addNodeAtPath(&root, path, std::make_unique<ExpressionASTNode>(expr));
            }

        } else if (elem.trueValue()) {
            if (!firstPositionalProjection) {
                FieldPath path(elem.fieldNameStringData());
                addNodeAtPath(&root, path, std::make_unique<BooleanConstantASTNode>(true));
            } else {
                uassert(31276,
                        "Cannot specify more than one positional projection per query.",
                        !hasPositional);

                uassert(31256, "Cannot specify positional operator and $elemMatch.", !hasElemMatch);

                StringData matchField = str::before(elem.fieldNameStringData(), '.');
                uassert(31277,
                        str::stream()
                            << "Positional projection '" << elem.fieldName() << "' does not "
                            << "match the query document.",
                        query && hasPositionalOperatorMatch(query, matchField));

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
                        findFirstPositionalOperator(remainingPathAfterPositional) == boost::none);

                // Get everything up to the first positional operator.
                StringData pathWithoutPositionalOperator =
                    elem.fieldNameStringData().substr(0, firstPositionalBegin);

                auto matcher =
                    CopyableMatchExpression{queryObj,
                                            expCtx,
                                            std::make_unique<ExtensionsCallbackNoop>(),
                                            MatchExpressionParser::kBanAllSpecialFeatures,
                                            true /* optimize expression */};

                FieldPath path(pathWithoutPositionalOperator);
                invariant(query);
                addNodeAtPath(&root,
                              path,
                              std::make_unique<ProjectionPositionalASTNode>(
                                  std::make_unique<MatchExpressionASTNode>(matcher)));

                hasPositional = true;
            }

            uassert(31253,
                    str::stream() << "Cannot do inclusion on field " << elem.fieldNameStringData()
                                  << " in exclusion projection",
                    !type || *type == ProjectType::kInclusion);
            type = ProjectType::kInclusion;
        } else {
            invariant(!elem.trueValue());
            FieldPath path(elem.fieldNameStringData());
            addNodeAtPath(&root, path, std::make_unique<BooleanConstantASTNode>(false));

            if (elem.fieldNameStringData() != "_id") {
                uassert(31254,
                        str::stream() << "Cannot do exclusion on field "
                                      << elem.fieldNameStringData() << "in inclusion projection",
                        !type || *type == ProjectType::kExclusion);
                type = ProjectType::kExclusion;
            }
        }
    }

    // find() defaults about inclusion/exclusion. These rules are preserved for compatibility
    // reasons. If there are no explicit inclusion/exclusion fields, the type depends on which
    // expressions (if any) are used.
    if (!type) {
        if (hasFindSlice) {
            type = ProjectType::kExclusion;
        } else if (hasElemMatch) {
            type = ProjectType::kInclusion;
        } else {
            // This happens only when the projection is entirely $meta expressions.
            type = ProjectType::kExclusion;
        }
    }
    invariant(type);

    if (!idSpecified && policies.idPolicy == ProjectionPolicies::DefaultIdPolicy::kIncludeId &&
        *type == ProjectType::kInclusion) {
        // Add a node to the root indicating that _id is included.
        addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(true));
    }

    return Projection{std::move(root), *type};
}

Projection parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                 const BSONObj& obj,
                 ProjectionPolicies policies) {
    return parse(std::move(expCtx), obj, nullptr, BSONObj(), std::move(policies));
}
}  // namespace projection_ast
}  // namespace mongo
