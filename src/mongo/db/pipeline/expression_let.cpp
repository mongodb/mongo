// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

namespace {

/**
 * Traverse Expression sub-tree and perfom two independent operations in a single traversal:
 * 1. For ExpressionFieldPath referencing a varialbe in constantVariables, replace it with
 *    ExpressionConstant.
 * 2. Fill usedVariables set with ids of all used variables. This is used later to remove unused
 *    variable declarations.
 */
[[nodiscard]] boost::intrusive_ptr<Expression> findUsedVariablesAndInlineConstants(
    boost::intrusive_ptr<Expression>& root,
    Variables& constantVariables,
    absl::flat_hash_set<Variables::Id>& usedVariables) {
    if (!root) {
        return root;
    }

    auto fieldPath = dynamic_cast<const ExpressionFieldPath*>(root.get());
    if (!fieldPath) {
        for (auto& child : root->getChildren()) {
            child = findUsedVariablesAndInlineConstants(child, constantVariables, usedVariables);
        }
        return root;
    }

    if (!fieldPath->isVariableReference()) {
        return root;
    }

    if (constantVariables.hasConstantValue(fieldPath->getVariableId())) {
        return make_intrusive<ExpressionConstant>(root->getExpressionContext(),
                                                  root->evaluate(Document{}, &constantVariables));
    } else {
        usedVariables.insert(fieldPath->getVariableId());
        return root;
    }
}

void removeUnusedVariables(ExpressionLet::VariableMap& variables,
                           const absl::flat_hash_set<Variables::Id>& usedVariables) {
    for (auto it = variables.begin(), end = variables.end(); it != end;) {
        if (!usedVariables.contains(it->first)) {
            it = variables.erase(it);
        } else {
            it++;
        }
    }
}

}  // namespace


REGISTER_STABLE_EXPRESSION(let, ExpressionLet::parse);
boost::intrusive_ptr<Expression> ExpressionLet::parse(ExpressionContext* const expCtx,
                                                      BSONElement expr,
                                                      const VariablesParseState& vpsIn) {
    MONGO_verify(expr.fieldNameStringData() == "$let");

    uassert(16874, "$let only supports an object as its argument", expr.type() == BSONType::object);
    const BSONObj args = expr.embeddedObject();

    // varsElem must be parsed before inElem regardless of BSON order.
    BSONElement varsElem;
    BSONElement inElem;
    for (auto&& arg : args) {
        if (arg.fieldNameStringData() == "vars") {
            varsElem = arg;
        } else if (arg.fieldNameStringData() == "in") {
            inElem = arg;
        } else {
            uasserted(16875,
                      str::stream() << "Unrecognized parameter to $let: " << arg.fieldName());
        }
    }

    uassert(16876, "Missing 'vars' parameter to $let", !varsElem.eoo());
    uassert(16877, "Missing 'in' parameter to $let", !inElem.eoo());

    // parse "vars"
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> letVariables;
    auto&& varsObj = varsElem.embeddedObjectUserCheck();
    for (auto&& varElem : varsObj) {
        letVariables.push_back({varElem.fieldName(), parseOperand(expCtx, varElem, vpsIn)});
    }

    return ExpressionLet::create(
        expCtx,
        std::move(letVariables),
        vpsIn,
        [&inElem](ExpressionContext* ctx, const VariablesParseState& vpsWithLetVars) {
            return parseOperand(ctx, inElem, vpsWithLetVars);
        });
}

boost::intrusive_ptr<Expression> ExpressionLet::create(
    ExpressionContext* expCtx,
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> letVariables,
    const VariablesParseState& vpsIn,
    CreateInExpr createInFunc) {
    // parse "vars"
    VariablesParseState vpsSub(vpsIn);  // vpsSub gets our vars, vpsIn doesn't.
    VariableMap vars;
    std::vector<boost::intrusive_ptr<Expression>> children;
    children.reserve(letVariables.size());
    for (auto&& [varName, expr] : letVariables) {
        children.push_back(std::move(expr));
    }

    // Make a place in the vector for "in".
    auto& inPtr = children.emplace_back(nullptr);

    std::vector<boost::intrusive_ptr<Expression>>::size_type index = 0;
    std::vector<Variables::Id> orderedVariableIds;
    for (const auto& [varName, movedExpr] : letVariables) {
        variableValidation::validateNameForUserWrite(varName);
        Variables::Id id = vpsSub.defineVariable(varName);

        orderedVariableIds.push_back(id);

        vars.emplace(id, NameAndExpression{varName, children[index]});  // only has outer vars
        ++index;
    }

    // create the "in" expression
    inPtr = createInFunc(expCtx, vpsSub);

    return new ExpressionLet(
        expCtx, std::move(vars), std::move(children), std::move(orderedVariableIds));
}

ExpressionLet::ExpressionLet(ExpressionContext* const expCtx,
                             VariableMap&& vars,
                             ExpressionVector&& children,
                             std::vector<Variables::Id> orderedVariableIds)
    : Expression(expCtx, std::move(children)),
      _kSubExpression(_children.size() - 1),
      _variables(std::move(vars)),
      _orderedVariableIds(std::move(orderedVariableIds)) {}

boost::intrusive_ptr<Expression> ExpressionLet::optimize() {
    if (_variables.empty()) {
        // we aren't binding any variables so just return the subexpression
        return _children[_kSubExpression]->optimize();
    }

    Variables constantVariables;
    for (VariableMap::iterator it = _variables.begin(), end = _variables.end(); it != end;) {
        it->second.expression = it->second.expression->optimize();
        if (auto constant = dynamic_cast<ExpressionConstant*>(it->second.expression.get())) {
            constantVariables.setConstantValue(it->first, constant->getValue());
            it = _variables.erase(it);
        } else {
            it++;
        }
    }

    absl::flat_hash_set<Variables::Id> usedVariables;
    _children[_kSubExpression] = findUsedVariablesAndInlineConstants(
        _children[_kSubExpression], constantVariables, usedVariables);
    removeUnusedVariables(_variables, usedVariables);

    _children[_kSubExpression] = _children[_kSubExpression]->optimize();
    return _variables.empty() ? _children[_kSubExpression] : this;
}

Value ExpressionLet::serialize(const query_shape::SerializationOptions& options) const {
    MutableDocument vars;
    for (VariableMap::const_iterator it = _variables.begin(), end = _variables.end(); it != end;
         ++it) {
        auto key = it->second.name;
        if (options.transformIdentifiers) {
            key = options.transformIdentifiersCallback(key);
        }
        vars[key] = it->second.expression->serialize(options);
    }

    return Value(DOC("$let" << DOC("vars" << vars.freeze() << "in"
                                          << _children[_kSubExpression]->serialize(options))));
}

Value ExpressionLet::evaluate(const Document& root,
                              Variables* variables,
                              const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

}  // namespace mongo
