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

#include "mongo/db/query/stage_builder/sbe/type_checker.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/util/assert_util.h"

#include <vector>

namespace mongo::stage_builder {
using namespace std::string_literals;

TypeChecker::TypeChecker() {
    // Define an initial binding level, so that the caller can define variable bindings before
    // invoking typeCheck().
    _bindings.emplace_back();
}

TypeChecker::TypeChecker(const TypeChecker& parent) {
    _bindings.emplace_back();
    // Import the type inferred for the variables the parent checker knows.
    for (auto it = parent._bindings.crbegin(); it != parent._bindings.crend(); it++) {
        for (auto varIt = it->cbegin(); varIt != it->cend(); varIt++) {
            _bindings.back().emplace(varIt->first, varIt->second);
        }
    }
}

TypeSignature TypeChecker::typeCheck(abt::ABT& node) {
    invariant(_bindings.size() == 1);
    return node.visit(*this, false);
}

boost::optional<TypeSignature> TypeChecker::getInferredType(abt::ProjectionName variable) {
    // Walk the list of active bindings until the variable is found.
    for (auto it = _bindings.rbegin(); it != _bindings.rend(); it++) {
        auto findIt = it->find(variable);
        if (findIt != it->end()) {
            return findIt->second;
        }
    }
    // No explicit type defined.
    return boost::none;
}

void TypeChecker::bind(abt::ProjectionName variable, TypeSignature type) {
    // Verify that the new type for the variable is compatible with the information deducted until
    // now.
    auto curType = getInferredType(variable);
    uassert(6950900, "Type checking error", !curType || type.isSubset(*curType));
    // Store the association in the current binding level.
    _bindings.back()[variable] = type;
}

void TypeChecker::enterLocalBinding() {
    // Add a new binding to the list of active ones.
    _bindings.emplace_back();
}

void TypeChecker::exitLocalBinding() {
    // We must never remove the top binding.
    invariant(_bindings.size() > 1);
    _bindings.pop_back();
}

TypeSignature TypeChecker::operator()(abt::ABT& node, abt::Constant& value, bool saveInference) {
    // A constant has a signature of the type of the value stored inside the node.
    auto [tag, _] = value.get();
    return getTypeSignature(tag);
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::Variable& var, bool saveInference) {
    // Retrieve the current type of the variable.
    return getInferredType(var.name()).value_or(TypeSignature::kAnyScalarType);
}

TypeSignature TypeChecker::operator()(abt::ABT& n,
                                      abt::LambdaAbstraction& lambda,
                                      bool saveInference) {
    // The Lambda node returns the value of its 'body' child.
    return lambda.getBody().visit(*this, false);
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::Let& let, bool saveInference) {
    // Define the new variable with the type of the 'bind' expression type.
    bind(let.varName(), let.bind().visit(*this, false));

    // The Let node returns the value of its 'in' child.
    TypeSignature resultType = let.in().visit(*this, false);

    // The current binding must be the one where we defined the variable.
    invariant(_bindings.back().contains(let.varName()));
    _bindings.back().erase(let.varName());

    return resultType;
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::MultiLet& multiLet, bool saveInference) {
    // Define the new variables with the type of the 'bind' expressions
    for (size_t idx = 0; idx < multiLet.numBinds(); ++idx) {
        bind(multiLet.varName(idx), multiLet.bind(idx).visit(*this, false));
    }

    // The MultiLet node returns the value of its 'in' child.
    TypeSignature resultType = multiLet.in().visit(*this, false);

    // The current binding must be the one where we defined the variables.
    for (auto&& name : multiLet.varNames()) {
        invariant(_bindings.back().contains(name));
        _bindings.back().erase(name);
    }

    return resultType;
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::UnaryOp& op, bool saveInference) {
    TypeSignature childType = op.getChild().visit(*this, false);
    switch (op.op()) {
        case abt::Operations::Not: {
            // The signature of Not is boolean plus Nothing if the operand can be Nothing.
            return TypeSignature::kBooleanType.include(
                childType.intersect(TypeSignature::kNothingType));
        } break;

        default:
            break;
    }
    return TypeSignature::kAnyScalarType;
}

// Recursively walk a binary node and invoke the callback with the arguments in the order of test.
// e.g. And(And(a,b), And(c,d)) will invoke the callback on a,b,c,d.
template <typename Callback>
void walkTreeInOrder(abt::BinaryOp* node, Callback callback) {
    auto& left = node->getLeftChild();
    if (auto ptr = left.cast<abt::BinaryOp>(); ptr && ptr->op() == node->op()) {
        walkTreeInOrder(ptr, callback);
    } else {
        callback(left);
    }
    auto& right = node->getRightChild();
    if (auto ptr = right.cast<abt::BinaryOp>(); ptr && ptr->op() == node->op()) {
        walkTreeInOrder(ptr, callback);
    } else {
        callback(right);
    }
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::BinaryOp& op, bool saveInference) {
    if (op.op() == abt::Operations::And) {
        // In an And operation, due to the short-circuiting logic, a child node
        // can infer some extra type information just because it is being evaluated
        // after another node that must have returned a 'true' value.
        // E.g. (exists(s4) && isNumber(s4)) can never be Nothing because the only place
        //      where Nothing can be returned is isNumber, but s4 cannot be Nothing because
        //      the first child of the And node had to return 'true' in order for isNumber to
        //      be executed, and this excludes the possibility that s4 is Nothing.

        // If we are requested *not* to preserve our inferences, define a local binding where the
        // variables used inside the And can be constrained as each test is assumed to succeed.
        // If the saveInference is true, we will be writing directly in the scope that our caller
        // set up.
        if (!saveInference) {
            enterLocalBinding();
        }

        bool canBeNothing = false;
        // Visit the logical children in their natural order, even if they are not direct
        // children of this node.
        walkTreeInOrder(&op, [&](abt::ABT& node) {
            // Visit the child node using the flag 'saveInference' set to true, so that any
            // constraint applied to a variable can be stored in the local binding.
            TypeSignature nodeType = node.visit(*this, true);
            canBeNothing |= TypeSignature::kNothingType.isSubset(nodeType);
        });

        if (!saveInference) {
            exitLocalBinding();
        }
        // The signature of the And is boolean plus Nothing if any operands can be Nothing.
        return canBeNothing ? TypeSignature::kBooleanType.include(TypeSignature::kNothingType)
                            : TypeSignature::kBooleanType;
    } else if (op.op() == abt::Operations::Or) {
        // Visit the logical children in their natural order, even if they are not direct
        // children of this node.
        bool canBeNothing = false;
        walkTreeInOrder(&op, [&](abt::ABT& node) {
            TypeSignature nodeType = node.visit(*this, false);
            canBeNothing |= TypeSignature::kNothingType.isSubset(nodeType);
        });
        // The signature of the Or is boolean plus Nothing if any operands can be Nothing.
        return canBeNothing ? TypeSignature::kBooleanType.include(TypeSignature::kNothingType)
                            : TypeSignature::kBooleanType;
    }

    TypeSignature lhs = op.getLeftChild().visit(*this, false);
    TypeSignature rhs = op.getRightChild().visit(*this, false);
    switch (op.op()) {
        case abt::Operations::FillEmpty: {
            // If the argument is already guaranteed not to be a Nothing, or the replacement value
            // is a Nothing itself, the fillEmpty can be removed.
            if (!TypeSignature::kNothingType.isSubset(lhs) ||
                rhs.isSubset(TypeSignature::kNothingType)) {
                swapAndUpdate(n, std::exchange(op.getLeftChild(), abt::make<abt::Blackhole>()));
                return lhs;
            }
            // The signature of FillEmtpy is the signature of the first argument, minus Nothing,
            // plus the signature of the second argument.
            return lhs.exclude(TypeSignature::kNothingType).include(rhs);
            break;
        }

        case abt::Operations::Add:
        case abt::Operations::Sub: {
            // The signature of the Add/Sub is either numeric or date, plus Nothing.
            auto argsType = lhs.include(rhs);
            return TypeSignature::kNumericType
                .include(argsType.intersect(TypeSignature::kDateTimeType))
                .include(argsType.intersect(TypeSignature::kNothingType));
        } break;

        case abt::Operations::Mult: {
            // The signature of the Mult is numeric plus Nothing.
            auto argsType = lhs.include(rhs);
            return TypeSignature::kNumericType.include(
                argsType.intersect(TypeSignature::kNothingType));
        } break;

        case abt::Operations::Eq: {
            if (op.getLeftChild().is<abt::Constant>() && !op.getRightChild().is<abt::Constant>()) {
                // Ensure we don't have a constant on the left side.
                std::swap(op.getLeftChild(), op.getRightChild());
                std::swap(lhs, rhs);
            }
            // Equality: check if one of the terms is a boolean constant, and remove it
            if (!op.getLeftChild().is<abt::Constant>() && op.getRightChild().is<abt::Constant>() &&
                lhs.isSubset(TypeSignature::kBooleanType.include(TypeSignature::kNothingType))) {
                // If the left side is type checked as a boolean and the right side is the
                // constant 'true', replace the comparison with just the left side; if it is
                // 'false', replace it with a not(left side).
                const auto [rhsTag, rhsVal] = op.getRightChild().cast<abt::Constant>()->get();

                if (rhsTag == sbe::value::TypeTags::Boolean) {
                    if (sbe::value::bitcastTo<bool>(rhsVal)) {
                        swapAndUpdate(
                            n, std::exchange(op.getLeftChild(), abt::make<abt::Blackhole>()));
                    } else {
                        swapAndUpdate(
                            n,
                            abt::make<abt::UnaryOp>(
                                abt::Operations::Not,
                                std::exchange(op.getLeftChild(), abt::make<abt::Blackhole>())));
                    }
                }
            }
            // The signature of the Eq is boolean plus Nothing when the types of the arguments are
            // not comparable.
            if (lhs.canCompareWith(rhs)) {
                return TypeSignature::kBooleanType;
            } else {
                return TypeSignature::kBooleanType.include(TypeSignature::kNothingType);
            }
        } break;

        case abt::Operations::Neq:
        case abt::Operations::Gt:
        case abt::Operations::Gte:
        case abt::Operations::Lt:
        case abt::Operations::Lte: {
            // The signature of comparison is boolean plus Nothing when the types of the arguments
            // are not comparable.
            if (lhs.canCompareWith(rhs)) {
                return TypeSignature::kBooleanType;
            } else {
                return TypeSignature::kBooleanType.include(TypeSignature::kNothingType);
            }
        } break;

        case abt::Operations::Cmp3w: {
            // The signature of comparison is integer plus Nothing when the types of the arguments
            // are not comparable.
            if (lhs.canCompareWith(rhs)) {
                return getTypeSignature(sbe::value::TypeTags::NumberInt32);
            } else {
                return getTypeSignature(sbe::value::TypeTags::NumberInt32)
                    .include(TypeSignature::kNothingType);
            }
        } break;

        default:
            break;
    }

    return TypeSignature::kAnyScalarType;
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::NaryOp& op, bool saveInference) {
    if (op.op() == abt::Operations::And) {
        // In an And operation, due to the short-circuiting logic, a child node
        // can infer some extra type information just because it is being evaluated
        // after another node that must have returned a 'true' value.
        // E.g. (exists(s4) && isNumber(s4)) can never be Nothing because the only place
        //      where Nothing can be returned is isNumber, but s4 cannot be Nothing because
        //      the first child of the And node had to return 'true' in order for isNumber to
        //      be executed, and this excludes the possibility that s4 is Nothing.

        // If we are requested *not* to preserve our inferences, define a local binding where the
        // variables used inside the And can be constrained as each test is assumed to succeed.
        // If the saveInference is true, we will be writing directly in the scope that our caller
        // set up.
        if (!saveInference) {
            enterLocalBinding();
        }

        bool canBeNothing = false;
        // Visit the logical children in their natural order.
        for (auto& node : op.nodes()) {
            // Visit the child node using the flag 'saveInference' set to true, so that any
            // constraint applied to a variable can be stored in the local binding.
            TypeSignature nodeType = node.visit(*this, true);
            canBeNothing |= TypeSignature::kNothingType.isSubset(nodeType);
        }

        if (!saveInference) {
            exitLocalBinding();
        }
        // The signature of the And is boolean plus Nothing if any operands can be Nothing.
        return canBeNothing ? TypeSignature::kBooleanType.include(TypeSignature::kNothingType)
                            : TypeSignature::kBooleanType;
    } else if (op.op() == abt::Operations::Or) {
        // Visit the logical children in their natural order.
        bool canBeNothing = false;
        for (auto& node : op.nodes()) {
            TypeSignature nodeType = node.visit(*this, false);
            canBeNothing |= TypeSignature::kNothingType.isSubset(nodeType);
        }
        // The signature of the Or is boolean plus Nothing if any operands can be Nothing.
        return canBeNothing ? TypeSignature::kBooleanType.include(TypeSignature::kNothingType)
                            : TypeSignature::kBooleanType;
    } else if (op.op() == abt::Operations::Add) {
        // The signature of the Add is either numeric or date, plus Nothing.
        TypeSignature sig = {};
        for (auto& node : op.nodes()) {
            TypeSignature nodeType = node.visit(*this, false);
            sig = sig.include(nodeType);
        }
        return TypeSignature::kNumericType.include(sig.intersect(TypeSignature::kDateTimeType))
            .include(sig.intersect(TypeSignature::kNothingType));
    } else if (op.op() == abt::Operations::Mult) {
        // The signature of the Mult is numeric plus Nothing.

        TypeSignature sig = {};
        for (auto& node : op.nodes()) {
            TypeSignature nodeType = node.visit(*this, false);
            sig = sig.include(nodeType);
        }
        return TypeSignature::kNumericType.include(sig.intersect(TypeSignature::kNothingType));
    }
    return TypeSignature::kAnyScalarType;
}

TypeSignature TypeChecker::evaluateTypeTest(abt::ABT& n,
                                            TypeSignature argSignature,
                                            TypeSignature typeToCheck) {
    if (argSignature.isSubset(TypeSignature::kNothingType)) {
        // If the argument is exactly Nothing, evaluate to Nothing
        swapAndUpdate(n, abt::Constant::nothing());
        return TypeSignature::kNothingType;
    } else if (argSignature.isSubset(typeToCheck)) {
        // If the argument is only one (or more) of the types to check, evaluate to True
        swapAndUpdate(n, abt::Constant::boolean(true));
        return TypeSignature::kBooleanType;
    } else if (!argSignature.containsAny(typeToCheck.include(TypeSignature::kNothingType))) {
        // If the argument doesn't include Nothing or any of the types to check, evaluate to False
        swapAndUpdate(n, abt::Constant::boolean(false));
        return TypeSignature::kBooleanType;
    }
    return TypeSignature::kBooleanType.include(argSignature.intersect(TypeSignature::kNothingType));
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::FunctionCall& op, bool saveInference) {
    size_t arity = op.nodes().size();
    if (arity == 3 && (op.name() == "traverseF"s || op.name() == "traverseP"s) &&
        op.nodes()[1].is<abt::LambdaAbstraction>()) {
        // Always process the last argument for completeness, but ignore its computed type.
        op.nodes()[2].visit(*this, false);

        TypeSignature argType = op.nodes()[0].visit(*this, false);

        // A traverseF/traverseP invoked with the first argument that is not an array will just
        // invoke the lambda expression on it, so we can remove it if we are assured that it
        // cannot possibly contain an array.
        if (!argType.containsAny(TypeSignature::kArrayType)) {
            auto lambda = op.nodes()[1].cast<abt::LambdaAbstraction>();
            // Define the lambda variable with the type of the 'bind' expression type.
            bind(lambda->varName(), argType);
            // Process the lambda knowing that its argument will be exactly the type we got from
            // processing the first argument.
            TypeSignature lambdaType = op.nodes()[1].visit(*this, false);
            // The current binding must be the one where we defined the variable.
            invariant(_bindings.back().contains(lambda->varName()));
            _bindings.back().erase(lambda->varName());

            swapAndUpdate(
                n,
                abt::make<abt::Let>(lambda->varName(),
                                    std::exchange(op.nodes()[0], abt::make<abt::Blackhole>()),
                                    std::exchange(lambda->getBody(), abt::make<abt::Blackhole>())));
            return lambdaType.include(argType.intersect(TypeSignature::kNothingType));
        }

        // The first argument could be an array, so the lambda will be invoked on multiple array
        // items of unknown type.
        op.nodes()[1].visit(*this, false);

        // Nothing can be inferred about the return type of traverseF()/traverseP() in this case.
        return TypeSignature::kAnyScalarType;
    }
    std::vector<TypeSignature> argTypes;
    argTypes.reserve(arity);
    for (auto& node : op.nodes()) {
        argTypes.emplace_back(node.visit(*this, false));
    }
    if (arity == 2) {
        if (op.name() == "typeMatch"s) {
            auto argSignature = argTypes[0];
            if (op.nodes()[1].is<abt::Constant>()) {
                auto [tagMask, valMask] = op.nodes()[1].cast<abt::Constant>()->get();
                if (tagMask == sbe::value::TypeTags::NumberInt32) {
                    auto bsonMask = static_cast<uint32_t>(sbe::value::bitcastTo<int32_t>(valMask));
                    if (!TypeSignature::kNothingType.isSubset(argSignature)) {
                        // See if we can answer the typeMatch call only using type inference. The
                        // type of the argument must be either completely inside or outside of the
                        // requested type mask in order to constant fold this call. It also must not
                        // include the possibility of being Nothing.
                        auto argTypes = getBSONTypesFromSignature(argSignature);
                        uint32_t argBsonTypeMask = 0;
                        for (auto tag : argTypes) {
                            argBsonTypeMask |= getBSONTypeMask(tag);
                        }
                        if ((argBsonTypeMask & ~bsonMask) == 0) {
                            swapAndUpdate(
                                n, abt::Constant::boolean((argBsonTypeMask & bsonMask) != 0));
                        } else if ((argBsonTypeMask & bsonMask) == 0) {
                            swapAndUpdate(n, abt::Constant::boolean(false));
                        }
                        return TypeSignature::kBooleanType;
                    }
                }
            }
            return TypeSignature::kBooleanType.include(
                argSignature.intersect(TypeSignature::kNothingType));
        }

        if (op.name() == "convert"s) {
            auto argSignature = argTypes[0];
            if (op.nodes()[1].is<abt::Constant>()) {
                auto [tagMask, valMask] = op.nodes()[1].cast<abt::Constant>()->get();
                if (tagMask == sbe::value::TypeTags::NumberInt32) {
                    sbe::value::TypeTags targetTypeTag =
                        (sbe::value::TypeTags)sbe::value::bitcastTo<int32_t>(valMask);
                    TypeSignature targetSignature = getTypeSignature(targetTypeTag);
                    // If the argument is already of the requested type (or Nothing), remove the
                    // 'convert' call.
                    if (argSignature.isSubset(
                            targetSignature.include(TypeSignature::kNothingType))) {
                        swapAndUpdate(n, std::exchange(op.nodes()[0], abt::make<abt::Blackhole>()));
                    }
                    return targetSignature.include(
                        argSignature.intersect(TypeSignature::kNothingType));
                }
            }
            return TypeSignature::kNumericType.include(
                argSignature.intersect(TypeSignature::kNothingType));
        }
    } else if (arity == 1) {
        if (op.name() == "exists"s) {
            // If the argument is already guaranteed not to be a Nothing or if it is a constant, we
            // can evaluate it now.
            if (!TypeSignature::kNothingType.isSubset(argTypes[0])) {
                swapAndUpdate(n, abt::Constant::boolean(true));
            } else if (saveInference && op.nodes()[0].cast<abt::Variable>()) {
                // If this 'exists' is testing a variable and is part of an And, add a mask
                // excluding Nothing from the type information of the variable.
                auto& varName = op.nodes()[0].cast<abt::Variable>()->name();
                auto varType = getInferredType(varName).value_or(TypeSignature::kAnyScalarType);

                bind(varName, varType.exclude(TypeSignature::kNothingType));
            }
            return TypeSignature::kBooleanType;
        }

        if (op.name() == "coerceToBool"s) {
            auto argSignature = argTypes[0];
            // If the argument is already guaranteed to be a boolean or a Nothing, the coerceToBool
            // is unnecessary.
            if (argSignature.isSubset(
                    TypeSignature::kBooleanType.include(TypeSignature::kNothingType))) {
                swapAndUpdate(n, std::exchange(op.nodes()[0], abt::make<abt::Blackhole>()));
            }
            return TypeSignature::kBooleanType.include(
                argSignature.intersect(TypeSignature::kNothingType));
        }

        if (op.name() == "isArray"s) {
            return evaluateTypeTest(n, argTypes[0], TypeSignature::kArrayType);
        }

        if (op.name() == "isDate"s) {
            return evaluateTypeTest(n, argTypes[0], getTypeSignature(sbe::value::TypeTags::Date));
        }

        if (op.name() == "isNull"s) {
            return evaluateTypeTest(n, argTypes[0], getTypeSignature(sbe::value::TypeTags::Null));
        }

        if (op.name() == "isNumber"s) {
            return evaluateTypeTest(n, argTypes[0], TypeSignature::kNumericType);
        }

        if (op.name() == "isObject"s) {
            return evaluateTypeTest(n, argTypes[0], TypeSignature::kObjectType);
        }

        if (op.name() == "isString"s) {
            return evaluateTypeTest(n, argTypes[0], TypeSignature::kStringType);
        }

        if (op.name() == "isTimestamp"s) {
            return evaluateTypeTest(
                n, argTypes[0], getTypeSignature(sbe::value::TypeTags::Timestamp));
        }
    }

    if ((arity == 6 && op.name() == "dateTrunc"s) || (arity == 5 && op.name() == "dateAdd"s)) {
        // Always mark Nothing as a possible return type, as it can be reported due to invalid
        // arguments.
        return getTypeSignature(sbe::value::TypeTags::Date).include(TypeSignature::kNothingType);
    }

    if ((arity == 5 || arity == 6) && op.name() == "dateDiff"s) {
        return getTypeSignature(sbe::value::TypeTags::NumberInt64)
            .include(TypeSignature::kNothingType);
    }

    if (arity == 0 && op.name() == "currentDate"s) {
        return getTypeSignature(sbe::value::TypeTags::Date);
    }

    return TypeSignature::kAnyScalarType;
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::If& op, bool saveInference) {
    // Define a new binding where the variables used inside the condition can be constrained by the
    // assumption that the condition is either true or false.
    enterLocalBinding();

    TypeSignature condType = op.getCondChild().visit(*this, true);
    TypeSignature thenType = op.getThenChild().visit(*this, false);

    // Remove the binding associated with the condition being true.
    exitLocalBinding();

    TypeSignature elseType = op.getElseChild().visit(*this, false);

    // The signature of If is the mix of both branches, plus Nothing if the condition can produce
    // it.
    return thenType.include(elseType).include(condType.intersect(TypeSignature::kNothingType));
}

TypeSignature TypeChecker::operator()(abt::ABT& n, abt::Switch& op, bool saveInference) {
    TypeSignature globalType;
    for (size_t i = 0; i < op.getNumBranches(); i++) {
        // Define a new binding where the variables used inside the condition can be constrained by
        // the assumption that the condition is either true or false.
        enterLocalBinding();

        TypeSignature condType = op.getCondChild(i).visit(*this, true);
        TypeSignature thenType = op.getThenChild(i).visit(*this, false);

        // Remove the binding associated with the condition being true.
        exitLocalBinding();
        // The signature of Switch is the mix of all branches, plus Nothing if the condition can
        // produce it.
        globalType =
            globalType.include(thenType).include(condType.intersect(TypeSignature::kNothingType));
    }

    TypeSignature elseType = op.getDefaultChild().visit(*this, false);
    return globalType.include(elseType);
}

void TypeChecker::swapAndUpdate(abt::ABT& n, abt::ABT newN) {
    // Do the swap.
    std::swap(n, newN);

    // newN now contains the old ABT and will be destroyed upon exiting this function.

    _changed = true;
}

}  // namespace mongo::stage_builder
