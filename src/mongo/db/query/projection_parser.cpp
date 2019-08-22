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
#include "mongo/db/pipeline/document.h"

namespace mongo {
namespace projection_ast {
namespace {
// TODO: SERVER-42421 Replace BadValue with numeric error code throughout this file.

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
    if (childPathNode == nullptr) {
        uasserted(31249,
                  str::stream() << "Path collision at " << path.fullPath() << " remaining portion "
                                << path.tail().fullPath());
    }

    addNodeAtPathHelper(childPathNode, path, componentIndex + 1, std::move(newChild));
}

void addNodeAtPath(ProjectionPathASTNode* root,
                   const FieldPath& path,
                   std::unique_ptr<ASTNode> newChild) {
    addNodeAtPathHelper(root, path, 0, std::move(newChild));
}

bool isPositionalOperator(const char* fieldName) {
    // TODO: SERVER-42421: Deal with special cases of $id, $ref, and $db.
    return str::endsWith(fieldName, ".$");
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

    // TODO: SERVER-42421 Support agg syntax with nesting.

    ProjectionPathASTNode root;

    bool idSpecified = false;

    bool hasPositional = false;
    bool hasElemMatch = false;
    boost::optional<ProjectType> type;
    for (auto&& elem : obj) {
        idSpecified |=
            elem.fieldNameStringData() == "_id" || elem.fieldNameStringData().startsWith("_id.");

        if (elem.type() == BSONType::Object) {
            BSONObj obj = elem.embeddedObject();
            BSONElement e2 = obj.firstElement();

            // Before converting to FieldPath make sure this is not a positional operator.
            uassert(
                ErrorCodes::BadValue,
                "$slice and positional projection are not allowed together",
                !(isPositionalOperator(elem.fieldName()) && e2.fieldNameStringData() == "$slice"));

            FieldPath path(elem.fieldNameStringData());

            if (e2.fieldNameStringData() == "$slice") {
                if (e2.isNumber()) {
                    // This is A-OK.
                    addNodeAtPath(
                        &root,
                        path,
                        std::make_unique<ProjectionSliceASTNode>(boost::none, e2.numberInt()));
                } else if (e2.type() == BSONType::Array) {
                    BSONObj arr = e2.embeddedObject();
                    if (2 != arr.nFields()) {
                        uasserted(ErrorCodes::BadValue, "$slice array wrong size");
                    }

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


                    if (limitElt.numberInt() <= 0) {
                        uasserted(31259,
                                  str::stream() << "$slice limit must be positive, got "
                                                << limitElt.numberInt());
                    }
                    addNodeAtPath(&root,
                                  path,
                                  std::make_unique<ProjectionSliceASTNode>(skipElt.numberInt(),
                                                                           limitElt.numberInt()));
                } else {
                    uasserted(ErrorCodes::BadValue,
                              "$slice only supports numbers and [skip, limit] arrays");
                }
            } else if (e2.fieldNameStringData() == "$elemMatch") {
                // Validate $elemMatch arguments and dependencies.
                if (BSONType::Object != e2.type()) {
                    uasserted(ErrorCodes::BadValue,
                              "elemMatch: Invalid argument, object required.");
                }

                if (hasPositional) {
                    uasserted(31255, "Cannot specify positional operator and $elemMatch.");
                }

                if (str::contains(elem.fieldName(), '.')) {
                    uasserted(ErrorCodes::BadValue,
                              "Cannot use $elemMatch projection on a nested field.");
                }

                // Create a MatchExpression for the elemMatch.
                BSONObj elemMatchObj = elem.wrap();
                invariant(elemMatchObj.isOwned());

                StatusWithMatchExpression statusWithMatcher =
                    MatchExpressionParser::parse(elemMatchObj,
                                                 expCtx,
                                                 ExtensionsCallbackNoop(),
                                                 MatchExpressionParser::kBanAllSpecialFeatures);
                auto matcher = uassertStatusOK(std::move(statusWithMatcher));

                auto matchNode =
                    std::make_unique<MatchExpressionASTNode>(elemMatchObj, std::move(matcher));

                addNodeAtPath(&root,
                              path,
                              std::make_unique<ProjectionElemMatchASTNode>(std::move(matchNode)));
                hasElemMatch = true;
            } else {
                uassert(31252,
                        "Cannot use expression in exclusion projection",
                        !type || *type == ProjectType::kInclusion);
                type = ProjectType::kInclusion;

                auto expr = Expression::parseExpression(expCtx, obj, expCtx->variablesParseState);
                addNodeAtPath(&root, path, std::make_unique<ExpressionASTNode>(expr));
            }

        } else if (elem.trueValue()) {
            if (!isPositionalOperator(elem.fieldName())) {
                FieldPath path(elem.fieldNameStringData());
                addNodeAtPath(&root, path, std::make_unique<BooleanConstantASTNode>(true));
            } else {
                if (hasPositional) {
                    uasserted(ErrorCodes::BadValue,
                              "Cannot specify more than one positional proj. per query.");
                }

                if (hasElemMatch) {
                    uasserted(31256, "Cannot specify positional operator and $elemMatch.");
                }

                StringData after = str::after(elem.fieldNameStringData(), ".$");
                if (after.find(".$"_sd) != std::string::npos) {
                    str::stream ss;
                    ss << "Positional projection '" << elem.fieldName() << "' contains "
                       << "the positional operator more than once.";
                    uasserted(ErrorCodes::BadValue, ss);
                }

                StringData matchField = str::before(elem.fieldNameStringData(), '.');
                if (!query || !hasPositionalOperatorMatch(query, matchField)) {
                    str::stream ss;
                    ss << "Positional projection '" << elem.fieldName() << "' does not "
                       << "match the query document.";
                    uasserted(ErrorCodes::BadValue, ss);
                }

                FieldPath path(str::before(elem.fieldNameStringData(), ".$"));
                invariant(query);
                addNodeAtPath(
                    &root,
                    path,
                    std::make_unique<ProjectionPositionalASTNode>(
                        std::make_unique<MatchExpressionASTNode>(queryObj, query->shallowClone())));

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

    if (!idSpecified && policies.idPolicy == ProjectionPolicies::DefaultIdPolicy::kIncludeId) {
        // Add a node to the root indicating that _id is included.
        addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(true));
    }

    return Projection{std::move(root), type ? *type : ProjectType::kExclusion};
}

Projection parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                 const BSONObj& obj,
                 ProjectionPolicies policies) {
    return parse(std::move(expCtx), obj, nullptr, BSONObj(), std::move(policies));
}
}  // namespace projection_ast
}  // namespace mongo
