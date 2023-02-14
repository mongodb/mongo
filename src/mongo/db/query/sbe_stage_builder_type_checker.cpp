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

#include "mongo/db/query/sbe_stage_builder_type_checker.h"

#include <stack>

#include "mongo/db/query/optimizer/syntax/expr.h"

namespace mongo::stage_builder {

// Return the signature corresponding to the given SBE type.
TypeSignature getTypeSignature(sbe::value::TypeTags type) {
    uint8_t tagIndex = static_cast<uint8_t>(type);
    return TypeSignature{1LL << tagIndex};
}

template <typename Head, typename... Tail>
TypeSignature getTypeSignature(Head type, Tail... tail) {
    return getTypeSignature(type).include(getTypeSignature(tail...));
}

// Return the set of SBE types encoded in the provided signature.
std::vector<sbe::value::TypeTags> getBSONTypesFromSignature(TypeSignature signature) {
    // This constant signature holds all the types that have a BSON counterpart and can
    // represent a value stored in the database, excluding all the TypeTags that describe
    // internal types like SortSpec, TimeZoneDB, etc...
    static TypeSignature kAnyBSONType = getTypeSignature(sbe::value::TypeTags::Nothing,
                                                         sbe::value::TypeTags::NumberInt32,
                                                         sbe::value::TypeTags::NumberInt64,
                                                         sbe::value::TypeTags::NumberDouble,
                                                         sbe::value::TypeTags::NumberDecimal,
                                                         sbe::value::TypeTags::Date,
                                                         sbe::value::TypeTags::Timestamp,
                                                         sbe::value::TypeTags::Boolean,
                                                         sbe::value::TypeTags::Null,
                                                         sbe::value::TypeTags::StringSmall,
                                                         sbe::value::TypeTags::StringBig,
                                                         sbe::value::TypeTags::Array,
                                                         sbe::value::TypeTags::ArraySet,
                                                         sbe::value::TypeTags::Object,
                                                         sbe::value::TypeTags::ObjectId,
                                                         sbe::value::TypeTags::MinKey,
                                                         sbe::value::TypeTags::MaxKey,
                                                         sbe::value::TypeTags::bsonObject,
                                                         sbe::value::TypeTags::bsonArray,
                                                         sbe::value::TypeTags::bsonString,
                                                         sbe::value::TypeTags::bsonSymbol,
                                                         sbe::value::TypeTags::bsonObjectId,
                                                         sbe::value::TypeTags::bsonBinData,
                                                         sbe::value::TypeTags::bsonUndefined,
                                                         sbe::value::TypeTags::bsonRegex,
                                                         sbe::value::TypeTags::bsonJavascript,
                                                         sbe::value::TypeTags::bsonDBPointer,
                                                         sbe::value::TypeTags::bsonCodeWScope);
    signature = signature.intersect(kAnyBSONType);
    std::vector<sbe::value::TypeTags> tags;
    for (size_t i = 0; i < sizeof(size_t) * 8; i++) {
        auto tag = static_cast<sbe::value::TypeTags>(i);
        if (getTypeSignature(tag).isSubset(signature)) {
            tags.push_back(tag);
        }
    }
    return tags;
}

TypeSignature TypeChecker::kAnyType = TypeSignature{~0};
TypeSignature TypeChecker::kArrayType = getTypeSignature(
    sbe::value::TypeTags::Array, sbe::value::TypeTags::ArraySet, sbe::value::TypeTags::bsonArray);
TypeSignature TypeChecker::kBooleanType = getTypeSignature(sbe::value::TypeTags::Boolean);
TypeSignature TypeChecker::kDateTimeType =
    getTypeSignature(sbe::value::TypeTags::Date, sbe::value::TypeTags::Timestamp);
TypeSignature TypeChecker::kNothingType = getTypeSignature(sbe::value::TypeTags::Nothing);
TypeSignature TypeChecker::kNumericType = getTypeSignature(sbe::value::TypeTags::NumberInt32,
                                                           sbe::value::TypeTags::NumberInt64,
                                                           sbe::value::TypeTags::NumberDecimal,
                                                           sbe::value::TypeTags::NumberDouble);
TypeSignature TypeChecker::kStringType = getTypeSignature(sbe::value::TypeTags::StringSmall,
                                                          sbe::value::TypeTags::StringBig,
                                                          sbe::value::TypeTags::bsonString);
TypeSignature TypeChecker::kObjectType =
    getTypeSignature(sbe::value::TypeTags::Object, sbe::value::TypeTags::bsonObject);

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

TypeSignature TypeChecker::typeCheck(optimizer::ABT& node) {
    invariant(_bindings.size() == 1);
    return node.visit(*this, false);
}

TypeSignature TypeChecker::getInferredType(optimizer::ProjectionName variable) {
    // Walk the list of active bindings until the variable is found.
    for (auto it = _bindings.rbegin(); it != _bindings.rend(); it++) {
        auto findIt = it->find(variable);
        if (findIt != it->end()) {
            return findIt->second;
        }
    }
    // No explicit type defined, return the wildcard type.
    return kAnyType;
}

void TypeChecker::bind(optimizer::ProjectionName variable, TypeSignature type) {
    // Verify that the new type for the variable is compatible with the information deducted until
    // now.
    TypeSignature curType = getInferredType(variable);
    uassert(6950900, "Type checking error", type.isSubset(curType));
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

TypeSignature TypeChecker::operator()(optimizer::ABT& node,
                                      optimizer::Constant& value,
                                      bool saveInference) {
    // A constant has a signature of the type of the value stored inside the node.
    auto [tag, _] = value.get();
    return getTypeSignature(tag);
}

TypeSignature TypeChecker::operator()(optimizer::ABT& n,
                                      optimizer::Variable& var,
                                      bool saveInference) {
    // Retrieve the current type of the variable.
    return getInferredType(var.name());
}

TypeSignature TypeChecker::operator()(optimizer::ABT& n, optimizer::Let& let, bool saveInference) {
    // Define the new variable with the type of the 'bind' expression type.
    bind(let.varName(), const_cast<optimizer::ABT&>(let.bind()).visit(*this, false));

    // The Let node returns the value of its 'in' child.
    TypeSignature resultType = const_cast<optimizer::ABT&>(let.in()).visit(*this, false);

    // The current binding must be the one where we defined the variable.
    invariant(_bindings.back().contains(let.varName()));
    _bindings.back().erase(let.varName());

    return resultType;
}

TypeSignature TypeChecker::operator()(optimizer::ABT& n,
                                      optimizer::UnaryOp& op,
                                      bool saveInference) {
    TypeSignature childType = const_cast<optimizer::ABT&>(op.getChild()).visit(*this, false);
    switch (op.op()) {
        case optimizer::Operations::Not: {
            // The signature of Not is boolean plus Nothing if the operand can be Nothing.
            return kBooleanType.include(childType.intersect(kNothingType));
        } break;

        default:
            break;
    }
    return kAnyType;
}

// Recursively walk a binary node and invoke the callback with the arguments in the order of test.
// e.g. And(And(a,b), And(c,d)) will invoke the callback on a,b,c,d.
template <typename Callback>
void walkTreeInOrder(optimizer::BinaryOp* node, Callback callback) {
    auto& left = const_cast<optimizer::ABT&>(node->getLeftChild());
    if (auto ptr = left.cast<optimizer::BinaryOp>(); ptr && ptr->op() == node->op()) {
        walkTreeInOrder(ptr, callback);
    } else {
        callback(left);
    }
    auto& right = const_cast<optimizer::ABT&>(node->getRightChild());
    if (auto ptr = right.cast<optimizer::BinaryOp>(); ptr && ptr->op() == node->op()) {
        walkTreeInOrder(ptr, callback);
    } else {
        callback(right);
    }
}

TypeSignature TypeChecker::operator()(optimizer::ABT& n,
                                      optimizer::BinaryOp& op,
                                      bool saveInference) {
    if (op.op() == optimizer::Operations::And) {
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
        walkTreeInOrder(&op, [&](optimizer::ABT& node) {
            // Visit the child node using the flag 'saveInference' set to true, so that any
            // constraint applied to a variable can be stored in the local binding.
            TypeSignature nodeType = node.visit(*this, true);
            canBeNothing |= kNothingType.isSubset(nodeType);
        });

        if (!saveInference) {
            exitLocalBinding();
        }
        // The signature of the And is boolean plus Nothing if any operands can be Nothing.
        return canBeNothing ? kBooleanType.include(kNothingType) : kBooleanType;
    } else if (op.op() == optimizer::Operations::Or) {
        // Visit the logical children in their natural order, even if they are not direct
        // children of this node.
        bool canBeNothing = false;
        walkTreeInOrder(&op, [&](optimizer::ABT& node) {
            TypeSignature nodeType = node.visit(*this, false);
            canBeNothing |= kNothingType.isSubset(nodeType);
        });
        // The signature of the Or is boolean plus Nothing if any operands can be Nothing.
        return canBeNothing ? kBooleanType.include(kNothingType) : kBooleanType;
    }

    TypeSignature lhs = const_cast<optimizer::ABT&>(op.getLeftChild()).visit(*this, false);
    TypeSignature rhs = const_cast<optimizer::ABT&>(op.getRightChild()).visit(*this, false);
    switch (op.op()) {
        case optimizer::Operations::FillEmpty: {
            // If the argument is already guaranteed not to be a Nothing, the fillEmpty can be
            // removed.
            if (!kNothingType.isSubset(lhs)) {
                swapAndUpdate(n,
                              std::exchange(const_cast<optimizer::ABT&>(op.getLeftChild()),
                                            optimizer::make<optimizer::Blackhole>()));
                return lhs;
            }
            // The signature of FillEmtpy is the signature of the first argument, minus Nothing,
            // plus the signature of the second argument.
            return lhs.exclude(kNothingType).include(rhs);
            break;
        }

        case optimizer::Operations::Add:
        case optimizer::Operations::Sub: {
            // The signature of the Add/Sub is either numeric or date, plus Nothing.
            auto argsType = lhs.include(rhs);
            return kNumericType.include(argsType.intersect(kDateTimeType))
                .include(argsType.intersect(kNothingType));
        } break;

        case optimizer::Operations::Mult: {
            // The signature of the Mult is numeric plus Nothing.
            auto argsType = lhs.include(rhs);
            return kNumericType.include(argsType.intersect(kNothingType));
        } break;

        case optimizer::Operations::Eq: {
            // Equality: check if one of the terms is a boolean constant, and remove it
            if (op.getLeftChild().is<optimizer::Constant>() &&
                !op.getRightChild().is<optimizer::Constant>()) {
                // Ensure we don't have a constant on the left side.
                std::swap(const_cast<optimizer::ABT&>(op.getLeftChild()),
                          const_cast<optimizer::ABT&>(op.getRightChild()));
                std::swap(lhs, rhs);
            }
            if (!op.getLeftChild().is<optimizer::Constant>() &&
                op.getRightChild().is<optimizer::Constant>() &&
                lhs.isSubset(kBooleanType.include(kNothingType))) {
                // If the left side is type checked as a boolean and the right side is the
                // constant 'true', replace the comparison with just the left side; if it is
                // 'false', replace it with a not(left side).
                const auto [rhsTag, rhsVal] = const_cast<optimizer::ABT&>(op.getRightChild())
                                                  .cast<optimizer::Constant>()
                                                  ->get();

                if (rhsTag == sbe::value::TypeTags::Boolean) {
                    if (sbe::value::bitcastTo<bool>(rhsVal)) {
                        swapAndUpdate(n,
                                      std::exchange(const_cast<optimizer::ABT&>(op.getLeftChild()),
                                                    optimizer::make<optimizer::Blackhole>()));
                    } else {
                        swapAndUpdate(
                            n,
                            optimizer::make<optimizer::UnaryOp>(
                                optimizer::Operations::Not,
                                std::exchange(const_cast<optimizer::ABT&>(op.getLeftChild()),
                                              optimizer::make<optimizer::Blackhole>())));
                    }
                }
            }
            // The signature of the Eq is boolean plus Nothing if either operands can be
            // Nothing.
            return kBooleanType.include(lhs.include(rhs).intersect(kNothingType));
        } break;

        case optimizer::Operations::Neq:
        case optimizer::Operations::Gt:
        case optimizer::Operations::Gte:
        case optimizer::Operations::Lt:
        case optimizer::Operations::Lte: {
            // The signature of comparison is boolean plus Nothing if either operands can be
            // Nothing.
            return kBooleanType.include(lhs.include(rhs).intersect(kNothingType));
        } break;

        default:
            break;
    }

    return kAnyType;
}

TypeSignature TypeChecker::evaluateTypeTest(optimizer::ABT& n,
                                            TypeSignature argSignature,
                                            TypeSignature typeToCheck) {
    if (argSignature.isSubset(kNothingType)) {
        // If the argument is exactly Nothing, evaluate to Nothing
        swapAndUpdate(n, optimizer::Constant::nothing());
        return kNothingType;
    } else if (argSignature.isSubset(typeToCheck)) {
        // If the argument is only one (or more) of the types to check, evaluate to True
        swapAndUpdate(n, optimizer::Constant::boolean(true));
        return kBooleanType;
    } else if (!argSignature.containsAny(typeToCheck.include(kNothingType))) {
        // If the argument doesn't include Nothing or any of the types to check, evaluate to False
        swapAndUpdate(n, optimizer::Constant::boolean(false));
        return kBooleanType;
    }
    return kBooleanType.include(argSignature.intersect(kNothingType));
}

TypeSignature TypeChecker::operator()(optimizer::ABT& n,
                                      optimizer::FunctionCall& op,
                                      bool saveInference) {
    size_t arity = op.nodes().size();
    std::vector<TypeSignature> argTypes;
    argTypes.reserve(arity);
    for (auto& node : op.nodes()) {
        argTypes.emplace_back(node.visit(*this, false));
    }
    if (op.name() == "exists" && arity == 1) {
        // If the argument is already guaranteed not to be a Nothing or if it is a constant, we can
        // evaluate it now.
        if (!kNothingType.isSubset(argTypes[0])) {
            swapAndUpdate(n, optimizer::Constant::boolean(true));
        } else if (saveInference && op.nodes()[0].cast<optimizer::Variable>()) {
            // If this 'exists' is testing a variable and is part of an And, add a mask excluding
            // Nothing from the type information of the variable.
            auto& varName = op.nodes()[0].cast<optimizer::Variable>()->name();
            bind(varName, getInferredType(varName).exclude(kNothingType));
        }
        return kBooleanType;
    }

    if (op.name() == "coerceToBool" && arity == 1) {
        auto argSignature = argTypes[0];
        // If the argument is already guaranteed to be a boolean or a Nothing, the coerceToBool is
        // unnecessary.
        if (argSignature.isSubset(kBooleanType.include(kNothingType))) {
            swapAndUpdate(n, std::exchange(op.nodes()[0], optimizer::make<optimizer::Blackhole>()));
        }
        return kBooleanType.include(argSignature.intersect(kNothingType));
    }

    if (op.name() == "typeMatch" && arity == 2) {
        auto argSignature = argTypes[0];
        if (op.nodes()[1].is<optimizer::Constant>()) {
            auto [tagMask, valMask] = op.nodes()[1].cast<optimizer::Constant>()->get();
            if (tagMask == sbe::value::TypeTags::NumberInt32) {
                int32_t bsonMask = sbe::value::bitcastTo<int32_t>(valMask);
                if (!kNothingType.isSubset(argSignature)) {
                    // See if we can answer the typeMatch call only using type inference. The type
                    // of the argument must be either completely inside or outside of the requested
                    // type mask in order to constant fold this call. It also must not include the
                    // possibility of being Nothing.
                    auto argTypes = getBSONTypesFromSignature(argSignature);
                    uint32_t argBsonTypeMask = 0;
                    for (auto tag : argTypes) {
                        argBsonTypeMask |= getBSONTypeMask(tag);
                    }
                    if ((argBsonTypeMask & ~bsonMask) == 0) {
                        swapAndUpdate(
                            n, optimizer::Constant::boolean((argBsonTypeMask & bsonMask) != 0));
                    } else if ((argBsonTypeMask & bsonMask) == 0) {
                        swapAndUpdate(n, optimizer::Constant::boolean(false));
                    }
                    return kBooleanType;
                }
            }
        }
        return kBooleanType.include(argSignature.intersect(kNothingType));
    }

    if (op.name() == "convert" && arity == 2) {
        auto argSignature = argTypes[0];
        if (op.nodes()[1].is<optimizer::Constant>()) {
            auto [tagMask, valMask] = op.nodes()[1].cast<optimizer::Constant>()->get();
            if (tagMask == sbe::value::TypeTags::NumberInt32) {
                sbe::value::TypeTags targetTypeTag =
                    (sbe::value::TypeTags)sbe::value::bitcastTo<int32_t>(valMask);
                TypeSignature targetSignature = getTypeSignature(targetTypeTag);
                // If the argument is already of the requested type (or Nothing), remove the
                // 'convert' call.
                if (argSignature.isSubset(targetSignature.include(kNothingType))) {
                    swapAndUpdate(
                        n, std::exchange(op.nodes()[0], optimizer::make<optimizer::Blackhole>()));
                }
                return targetSignature.include(argSignature.intersect(kNothingType));
            }
        }
        return kNumericType.include(argSignature.intersect(kNothingType));
    }

    if (op.name() == "isArray" && arity == 1) {
        return evaluateTypeTest(n, argTypes[0], kArrayType);
    }

    if (op.name() == "isDate" && arity == 1) {
        return evaluateTypeTest(n, argTypes[0], getTypeSignature(sbe::value::TypeTags::Date));
    }

    if (op.name() == "isNull" && arity == 1) {
        return evaluateTypeTest(n, argTypes[0], getTypeSignature(sbe::value::TypeTags::Null));
    }

    if (op.name() == "isNumber" && arity == 1) {
        return evaluateTypeTest(n, argTypes[0], kNumericType);
    }

    if (op.name() == "isObject" && arity == 1) {
        return evaluateTypeTest(n, argTypes[0], kDateTimeType);
    }

    if (op.name() == "isString" && arity == 1) {
        return evaluateTypeTest(n, argTypes[0], kStringType);
    }

    if (op.name() == "isTimestamp" && arity == 1) {
        return evaluateTypeTest(n, argTypes[0], getTypeSignature(sbe::value::TypeTags::Timestamp));
    }

    return kAnyType;
}

TypeSignature TypeChecker::operator()(optimizer::ABT& n, optimizer::If& op, bool saveInference) {
    // Define a new binding where the variables used inside the condition can be constrained by the
    // assumption that the condition is either true or false.
    enterLocalBinding();

    TypeSignature condType = const_cast<optimizer::ABT&>(op.getCondChild()).visit(*this, true);
    TypeSignature thenType = const_cast<optimizer::ABT&>(op.getThenChild()).visit(*this, false);

    // Remove the binding associated with the condition being true.
    exitLocalBinding();

    TypeSignature elseType = const_cast<optimizer::ABT&>(op.getElseChild()).visit(*this, false);

    // The signature of If is the mix of both branches, plus Nothing if the condition can produce
    // it.
    return thenType.include(elseType).include(condType.intersect(kNothingType));
}

void TypeChecker::swapAndUpdate(optimizer::ABT& n, optimizer::ABT newN) {
    // Do the swap.
    std::swap(n, newN);

    // newN now contains the old ABT and will be destroyed upon exiting this function.

    _changed = true;
}

}  // namespace mongo::stage_builder
