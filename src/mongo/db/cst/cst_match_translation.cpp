/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <algorithm>
#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <iterator>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_match_translation.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/util/visit_helper.h"

namespace mongo::cst_match_translation {
namespace {

std::unique_ptr<MatchExpression> translateMatchPredicate(
    const CNode::Fieldname& fieldName,
    const CNode& cst,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Walk an array of nodes and produce a vector of MatchExpressions.
 */
template <class Type>
std::unique_ptr<Type> translateTreeExpr(const CNode::ArrayChildren& array,
                                        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto expr = std::make_unique<Type>();
    for (auto&& node : array) {
        // Tree expressions require each element to be it's own match expression object.
        expr->add(translateMatchExpression(node, expCtx).release());
    }
    return expr;
}

std::unique_ptr<MatchExpression> translateNot(
    const CNode::Fieldname& fieldName,
    const CNode& argument,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // $not can accept a regex or an object expression.
    if (auto regex = stdx::get_if<UserRegex>(&argument.payload)) {
        auto regexExpr = std::make_unique<RegexMatchExpression>(
            stdx::get<UserFieldname>(fieldName), regex->pattern, regex->flags);
        return std::make_unique<NotMatchExpression>(std::move(regexExpr));
    }

    auto root = std::make_unique<AndMatchExpression>();
    root->add(translateMatchPredicate(fieldName, argument, expCtx).release());
    return std::make_unique<NotMatchExpression>(std::move(root));
}

std::unique_ptr<MatchExpression> translatePathExpression(
    const CNode::Fieldname& fieldName,
    const CNode::ObjectChildren& object,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    for (auto&& [op, argument] : object) {
        switch (stdx::get<KeyFieldname>(op)) {
            case KeyFieldname::notExpr:
                return translateNot(fieldName, argument, expCtx);
            default:
                MONGO_UNREACHABLE;
        }
    }
    MONGO_UNREACHABLE;
}

std::unique_ptr<MatchExpression> translateMatchPredicate(
    const CNode::Fieldname& fieldName,
    const CNode& cst,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (auto keyField = stdx::get_if<KeyFieldname>(&fieldName)) {
        // Top level match expression.
        switch (*keyField) {
            case KeyFieldname::andExpr:
                return translateTreeExpr<AndMatchExpression>(cst.arrayChildren(), expCtx);
            case KeyFieldname::orExpr:
                return translateTreeExpr<OrMatchExpression>(cst.arrayChildren(), expCtx);
            case KeyFieldname::norExpr:
                return translateTreeExpr<NorMatchExpression>(cst.arrayChildren(), expCtx);
            default:
                MONGO_UNREACHABLE;
        }
    } else {
        // Expression is over a user fieldname.
        return stdx::visit(
            visit_helper::Overloaded{
                [&](const CNode::ObjectChildren& userObject) -> std::unique_ptr<MatchExpression> {
                    return translatePathExpression(fieldName, userObject, expCtx);
                },
                [&](const CNode::ArrayChildren& userObject) -> std::unique_ptr<MatchExpression> {
                    MONGO_UNREACHABLE;
                },
                // Other types are always treated as equality predicates.
                [&](auto&& userValue) -> std::unique_ptr<MatchExpression> {
                    return std::make_unique<EqualityMatchExpression>(
                        StringData{stdx::get<UserFieldname>(fieldName)},
                        cst_pipeline_translation::translateLiteralLeaf(cst),
                        nullptr, /* TODO SERVER-49486: Add ErrorAnnotation for MatchExpressions */
                        expCtx->getCollator());
                }},
            cst.payload);
    }
    MONGO_UNREACHABLE;
}

}  // namespace

std::unique_ptr<MatchExpression> translateMatchExpression(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto root = std::make_unique<AndMatchExpression>();
    for (const auto& [fieldName, expr] : cst.objectChildren()) {
        root->add(translateMatchPredicate(fieldName, expr, expCtx).release());
    }
    return root;
}

}  // namespace mongo::cst_match_translation
