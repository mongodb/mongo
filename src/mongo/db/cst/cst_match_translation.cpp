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
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::cst_match_translation {
namespace {

std::unique_ptr<MatchExpression> translateMatchPredicate(
    const CNode::Fieldname& fieldName,
    const CNode& cst,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback);

std::unique_ptr<MatchExpression> translatePathExpression(const UserFieldname& fieldName,
                                                         const CNode::ObjectChildren& object);

/**
 * Walk an array of nodes and produce a vector of MatchExpressions.
 */
template <class Type>
std::unique_ptr<Type> translateTreeExpr(const CNode::ArrayChildren& array,
                                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        const ExtensionsCallback& extensionsCallback) {
    auto expr = std::make_unique<Type>();
    for (auto&& node : array) {
        // Tree expressions require each element to be it's own match expression object.
        expr->add(translateMatchExpression(node, expCtx, extensionsCallback));
    }
    return expr;
}

// Handles predicates of the form  <fieldname>: { $not: <argument> }
std::unique_ptr<MatchExpression> translateNot(const UserFieldname& fieldName,
                                              const CNode& argument) {
    // $not can accept a regex or an object expression.
    if (auto regex = stdx::get_if<UserRegex>(&argument.payload)) {
        auto regexExpr =
            std::make_unique<RegexMatchExpression>(fieldName, regex->pattern, regex->flags);
        return std::make_unique<NotMatchExpression>(std::move(regexExpr));
    }

    auto root = std::make_unique<AndMatchExpression>();
    root->add(
        translatePathExpression(fieldName, stdx::get<CNode::ObjectChildren>(argument.payload)));
    return std::make_unique<NotMatchExpression>(std::move(root));
}

std::unique_ptr<MatchExpression> translateExists(const CNode::Fieldname& fieldName,
                                                 const CNode& argument) {
    auto root = std::make_unique<ExistsMatchExpression>(stdx::get<UserFieldname>(fieldName));
    if (stdx::visit(OverloadedVisitor{[&](const UserLong& userLong) { return userLong != 0; },
                                      [&](const UserDouble& userDbl) { return userDbl != 0; },
                                      [&](const UserDecimal& userDc) {
                                          return userDc.isNotEqual(Decimal128(0));
                                      },
                                      [&](const UserInt& userInt) { return userInt != 0; },
                                      [&](const UserBoolean& b) { return b; },
                                      [&](const UserNull&) { return false; },
                                      [&](const UserUndefined&) { return false; },
                                      [&](auto&&) { return true; }},
                    argument.payload)) {
        return root;
    }
    return std::make_unique<NotMatchExpression>(root.release());
}

MatcherTypeSet getMatcherTypeSet(const CNode& argument) {
    MatcherTypeSet ts;
    auto add_individual_to_type_set = [&](const CNode& a) {
        return stdx::visit(
            OverloadedVisitor{
                [&](const UserLong& userLong) {
                    auto valueAsInt =
                        BSON("" << userLong).firstElement().parseIntegerElementToInt();
                    ts.bsonTypes.insert(static_cast<BSONType>(valueAsInt.getValue()));
                },
                [&](const UserDouble& userDbl) {
                    auto valueAsInt = BSON("" << userDbl).firstElement().parseIntegerElementToInt();
                    ts.bsonTypes.insert(static_cast<BSONType>(valueAsInt.getValue()));
                },
                [&](const UserDecimal& userDc) {
                    auto valueAsInt = BSON("" << userDc).firstElement().parseIntegerElementToInt();
                    ts.bsonTypes.insert(static_cast<BSONType>(valueAsInt.getValue()));
                },
                [&](const UserInt& userInt) {
                    auto valueAsInt = BSON("" << userInt).firstElement().parseIntegerElementToInt();
                    ts.bsonTypes.insert(static_cast<BSONType>(valueAsInt.getValue()));
                },
                [&](const UserString& s) {
                    if (StringData{s} == MatcherTypeSet::kMatchesAllNumbersAlias) {
                        ts.allNumbers = true;
                        return;
                    }
                    auto optValue = findBSONTypeAlias(s);
                    invariant(optValue);
                    ts.bsonTypes.insert(*optValue);
                },
                [&](auto&&) { MONGO_UNREACHABLE; }},
            a.payload);
    };
    if (auto children = stdx::get_if<CNode::ArrayChildren>(&argument.payload)) {
        for (auto child : (*children)) {
            add_individual_to_type_set(child);
        }
    } else {
        add_individual_to_type_set(argument);
    }
    return ts;
}

// Handles predicates of the form  <fieldname>: { ... }
// For example:
//   { abc: {$not: 5} }
//   { abc: {$eq: 0} }
//   { abc: {$gt: 0, $lt: 2} }
// Examples of predicates not handled here:
//   { abc: 5 }
//   { $expr: ... }
//   { $where: "return 1" }
// Note, this function does not require an ExpressionContext.
// The only MatchExpression that requires an ExpressionContext is $expr
// (if you include $where, which can desugar to $expr + $function).
std::unique_ptr<MatchExpression> translatePathExpression(const UserFieldname& fieldName,
                                                         const CNode::ObjectChildren& object) {
    for (auto&& [op, argument] : object) {
        switch (stdx::get<KeyFieldname>(op)) {
            case KeyFieldname::notExpr:
                return translateNot(fieldName, argument);
            case KeyFieldname::existsExpr:
                return translateExists(fieldName, argument);
            case KeyFieldname::type:
                return std::make_unique<TypeMatchExpression>(fieldName,
                                                             getMatcherTypeSet(argument));
            case KeyFieldname::matchMod: {
                const auto divisor =
                    stdx::get<CNode::ArrayChildren>(argument.payload)[0].numberInt();
                const auto remainder =
                    stdx::get<CNode::ArrayChildren>(argument.payload)[1].numberInt();
                return std::make_unique<ModMatchExpression>(fieldName, divisor, remainder);
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    MONGO_UNREACHABLE;
}

// Take a variant and either get (by copying) the T it holds, or construct a default value using
// the callable. For example:
//   getOr<int>(123, []() { return 0; }) == 123
//   getOr<int>("x", []() { return 0; }) == 0
template <class T, class V, class F>
T getOr(const V& myVariant, F makeDefaultValue) {
    if (auto* value = stdx::get_if<T>(&myVariant)) {
        return *value;
    } else {
        return makeDefaultValue();
    }
}

// Handles predicates of the form  <fieldname>: <anything>
// For example:
//   { abc: 5 }
//   { abc: {$lt: 5} }
// Examples of predicates not handled here:
//   { $where: "return 1" }
//   { $and: ... }
std::unique_ptr<MatchExpression> translateMatchPredicate(
    const CNode::Fieldname& fieldName,
    const CNode& cst,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback) {
    if (auto keyField = stdx::get_if<KeyFieldname>(&fieldName)) {
        // Top level match expression.
        switch (*keyField) {
            case KeyFieldname::andExpr:
                return translateTreeExpr<AndMatchExpression>(
                    cst.arrayChildren(), expCtx, extensionsCallback);
            case KeyFieldname::orExpr:
                return translateTreeExpr<OrMatchExpression>(
                    cst.arrayChildren(), expCtx, extensionsCallback);
            case KeyFieldname::norExpr:
                return translateTreeExpr<NorMatchExpression>(
                    cst.arrayChildren(), expCtx, extensionsCallback);
            case KeyFieldname::commentExpr:
                // comment expr is not added to the tree.
                return nullptr;
            case KeyFieldname::expr: {
                // The ExprMatchExpression maintains (shared) ownership of expCtx,
                // which the Expression from translateExpression depends on.
                return std::make_unique<ExprMatchExpression>(
                    cst_pipeline_translation::translateExpression(
                        cst, expCtx.get(), expCtx->variablesParseState),
                    expCtx);
            }
            case KeyFieldname::text: {
                const auto& args = cst.objectChildren();
                dassert(verifyFieldnames(
                    {
                        KeyFieldname::caseSensitive,
                        KeyFieldname::diacriticSensitive,
                        KeyFieldname::language,
                        KeyFieldname::search,
                    },
                    args));

                TextMatchExpressionBase::TextParams params;
                params.caseSensitive = getOr<bool>(args[0].second.payload, []() {
                    return TextMatchExpressionBase::kCaseSensitiveDefault;
                });
                params.diacriticSensitive = getOr<bool>(args[1].second.payload, []() {
                    return TextMatchExpressionBase::kDiacriticSensitiveDefault;
                });
                params.language = getOr<std::string>(args[2].second.payload, []() { return ""s; });
                params.query = stdx::get<std::string>(args[3].second.payload);

                return extensionsCallback.createText(std::move(params));
            }
            case KeyFieldname::where: {
                std::string code;
                if (auto str = stdx::get_if<UserString>(&cst.payload)) {
                    code = *str;
                } else if (auto js = stdx::get_if<UserJavascript>(&cst.payload)) {
                    code = std::string{js->code};
                } else {
                    MONGO_UNREACHABLE;
                }
                return extensionsCallback.createWhere(expCtx, {std::move(code)});
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else {
        // Expression is over a user fieldname.
        return stdx::visit(
            OverloadedVisitor{
                [&](const CNode::ObjectChildren& userObject) -> std::unique_ptr<MatchExpression> {
                    return translatePathExpression(stdx::get<UserFieldname>(fieldName), userObject);
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
    const CNode& cst,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback) {

    auto root = std::make_unique<AndMatchExpression>();
    for (const auto& [fieldName, expr] : cst.objectChildren()) {
        // A nullptr for 'translatedExpression' indicates that the particular operator should not
        // be added to 'root'. The $comment operator currently follows this convention.
        if (auto translatedExpression =
                translateMatchPredicate(fieldName, expr, expCtx, extensionsCallback);
            translatedExpression) {
            root->add(std::move(translatedExpression));
        }
    }
    return root;
}

bool verifyFieldnames(const std::vector<CNode::Fieldname>& expected,
                      const std::vector<std::pair<CNode::Fieldname, CNode>>& actual) {
    if (expected.size() != actual.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i].first)
            return false;
    }
    return true;
}

}  // namespace mongo::cst_match_translation
