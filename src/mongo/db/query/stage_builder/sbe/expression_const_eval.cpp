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

#include "mongo/db/query/stage_builder/sbe/expression_const_eval.h"

#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

namespace mongo::stage_builder {
using namespace std::string_literals;
void ExpressionConstEval::optimize(abt::ABT& n) {
    invariant(_varRefs.empty());
    invariant(_singleRef.empty());
    invariant(!_inRefBlock);
    invariant(_inCostlyCtx == 0);
    invariant(_staleDefs.empty());
    invariant(_staleABTs.empty());

    _changed = false;

    // We run the transport<true> that will pass the reference to abt::ABT to specific
    // transport functions. The reference serves as a conceptual 'this' pointer allowing the
    // transport function to change the node itself.
    algebra::transport<true>(n, *this);
    invariant(_varRefs.empty());

    while (_changed) {
        if (_singleRef.empty()) {
            break;
        }
        _changed = false;
        algebra::transport<true>(n, *this);
    }

    // TODO: should we be clearing here?
    _singleRef.clear();

    _staleDefs.clear();
    _staleABTs.clear();
}

void ExpressionConstEval::transport(abt::ABT& n, const abt::Variable& var) {
    if (auto it = _variableDefinitions.find(var.name()); it != _variableDefinitions.end()) {
        auto& def = it->second;

        if (!def.definition.empty()) {
            // See if we have already manipulated this definition and if so then use the newer
            // version.
            if (auto it = _staleDefs.find(def.definition); it != _staleDefs.end()) {
                def.definition = it->second;
            }
            if (auto it = _staleDefs.find(def.definedBy); it != _staleDefs.end()) {
                def.definedBy = it->second;
            }

            if (auto constant = def.definition.cast<abt::Constant>(); constant && !_inRefBlock) {
                // If we find the definition and it is a simple constant then substitute the
                // variable.
                swapAndUpdate(n, def.definition.copy());
            } else if (auto variable = def.definition.cast<abt::Variable>();
                       variable && !_inRefBlock) {
                // This is an alias for another variable, replace it with a reference to the
                // original one and update its counter (if it is tracked, it could be a reference to
                // a slot).
                auto itLet = _varRefs.find(variable->name());
                if (itLet != _varRefs.end()) {
                    itLet->second++;
                }
                swapAndUpdate(n, def.definition.copy());
            } else if (_singleRef.erase(var.name())) {
                swapAndUpdate(n, def.definition.copy());
            } else if (def.definedBy.cast<abt::Let>() || def.definedBy.cast<abt::MultiLet>()) {
                auto itLet = _varRefs.find(var.name());
                tassert(10252300, "Found reference to undefined variable", itLet != _varRefs.end());
                itLet->second++;
            }
        }
    }
}

void ExpressionConstEval::prepare(abt::ABT& n, const abt::Let& let) {
    tassert(
        10252301, "Found duplicate variable definition", _varRefs.emplace(let.varName(), 0).second);
    _variableDefinitions.emplace(let.varName(), abt::Definition{n.ref(), let.bind().ref()});
}

void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::Let& let,
                                    abt::ABT& bind,
                                    abt::ABT& in) {
    auto itLet = _varRefs.find(let.varName());
    if (itLet->second == 0) {
        // The bind expressions has not been referenced so it is dead code and the whole let
        // expression can be removed; i.e. we implement a following rewrite:
        //
        // n == let var=<bind expr> in <in expr>
        //
        //     v
        //
        // n == <in expr>

        // We don't want to abt::make a copy of 'in' as it may be arbitrarily large. Also, we
        // cannot move it out as it is part of the Let object and we do not want to invalidate any
        // assumptions the Let may have about its structure. Hence we swap it for the "special"
        // Blackhole object. The Blackhole does nothing, it just plugs the hole left in the 'in'
        // place.
        auto result = std::exchange(in, abt::make<abt::Blackhole>());

        // Swap the current node (n) for the result.
        swapAndUpdate(n, std::move(result));
    } else if (itLet->second == 1) {
        // The bind expression has been referenced exactly once so schedule it for inlining.
        _singleRef.emplace(let.varName());
        _changed = true;
    }
    _varRefs.erase(itLet);
    _variableDefinitions.erase(let.varName());
}

void ExpressionConstEval::prepare(abt::ABT& n, const abt::MultiLet& multiLet) {
    for (size_t i = 0; i < multiLet.numBinds(); ++i) {
        const auto& varName = multiLet.varNames()[i];
        tassert(
            10130811, "Found duplicate variable definition", _varRefs.emplace(varName, 0).second);
        _variableDefinitions.emplace(varName, abt::Definition{n.ref(), multiLet.bind(i).ref()});
    }
}

void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::MultiLet& multiLet,
                                    std::vector<abt::ABT>& args) {
    auto& varNames = multiLet.varNames();
    if (std::all_of(varNames.begin(), varNames.end(), [&](auto&& varName) {
            return _varRefs.find(varName)->second == 0;
        })) {
        // None of the bind expressions have been referenced so it is dead code and the whole
        // multiLet expression can be removed
        auto result = std::exchange(args.back(), abt::make<abt::Blackhole>());

        // Swap the current node (n) for the result.
        swapAndUpdate(n, std::move(result));
    } else {
        if (std::any_of(varNames.begin(), varNames.end(), [&](auto&& varName) {
                return _varRefs.find(varName)->second == 0;
            })) {
            // Trim out the bind expressions which have not been referenced, then constant fold it
            std::vector<abt::ProjectionName> newVarNames;
            std::vector<abt::ABT> newNodes;

            for (size_t idx = 0; idx < multiLet.numBinds(); ++idx) {
                if (_varRefs[varNames[idx]] != 0) {
                    newVarNames.push_back(varNames[idx]);
                    newNodes.emplace_back(std::exchange(args[idx], abt::make<abt::Blackhole>()));
                }
            }
            newNodes.emplace_back(std::exchange(args.back(), abt::make<abt::Blackhole>()));
            auto trimmedMultiLet =
                abt::make<abt::MultiLet>(std::move(newVarNames), std::move(newNodes));
            swapAndUpdate(n, trimmedMultiLet);
        }

        auto& mayBeTrimmedVarNames = n.cast<abt::MultiLet>()->varNames();

        // For the bind expressions which have been referenced exactly once, schedule them for
        // inlining.
        std::for_each(
            mayBeTrimmedVarNames.begin(), mayBeTrimmedVarNames.end(), [&](auto&& varName) {
                if (_varRefs.find(varName)->second == 1) {
                    _singleRef.emplace(varName);
                    _changed = true;
                }
            });
    }

    for (auto&& name : varNames) {
        _varRefs.erase(name);
        _variableDefinitions.erase(name);
    }
}

void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::LambdaApplication& app,
                                    abt::ABT& lam,
                                    abt::ABT& arg) {
    // If the 'lam' expression is abt::LambdaAbstraction then we can do the inplace beta
    // reduction.
    // TODO - missing alpha conversion so for now assume globally unique names.
    if (auto lambda = lam.cast<abt::LambdaAbstraction>(); lambda) {
        auto result =
            abt::make<abt::Let>(lambda->varName(),
                                std::exchange(arg, abt::make<abt::Blackhole>()),
                                std::exchange(lambda->getBody(), abt::make<abt::Blackhole>()));

        swapAndUpdate(n, std::move(result));
    }
}

void ExpressionConstEval::transport(abt::ABT& n, const abt::UnaryOp& op, abt::ABT& child) {
    switch (op.op()) {
        case abt::Operations::Not: {
            if (const auto childConst = child.cast<abt::Constant>();
                childConst && childConst->isValueBool()) {
                swapAndUpdate(n, abt::Constant::boolean(!childConst->getValueBool()));
            }
            break;
        }
        case abt::Operations::Neg: {
            // Negation is implemented as a subtraction from 0.
            if (const auto childConst = child.cast<abt::Constant>(); childConst) {
                auto [tag, value] = childConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericSub(sbe::value::TypeTags::NumberInt32,
                                           sbe::value::bitcastFrom<int32_t>(0),
                                           tag,
                                           value);
                swapAndUpdate(n, abt::make<abt::Constant>(resultType, resultValue));
            }
            break;
        }
        default:
            break;
    }
}

// Specific transport for binary operation
// The const correctness is probably wrong (as const abt::ABT& lhs, const abt::ABT& rhs
// does not work for some reason but we can fix it later).
void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::BinaryOp& op,
                                    abt::ABT& lhs,
                                    abt::ABT& rhs) {

    switch (op.op()) {
        case abt::Operations::Add: {
            // Let say we want to recognize ConstLhs + ConstRhs and replace it with the result of
            // addition.
            auto lhsConst = lhs.cast<abt::Constant>();
            auto rhsConst = rhs.cast<abt::Constant>();
            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericAdd(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, abt::make<abt::Constant>(resultType, resultValue));
            }
            break;
        }

        case abt::Operations::Sub: {
            // Let say we want to recognize ConstLhs - ConstRhs and replace it with the result of
            // subtraction.
            auto lhsConst = lhs.cast<abt::Constant>();
            auto rhsConst = rhs.cast<abt::Constant>();

            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericSub(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, abt::make<abt::Constant>(resultType, resultValue));
            }
            break;
        }

        case abt::Operations::Mult: {
            // Let say we want to recognize ConstLhs * ConstRhs and replace it with the result of
            // multiplication.
            auto lhsConst = lhs.cast<abt::Constant>();
            auto rhsConst = rhs.cast<abt::Constant>();

            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericMul(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, abt::make<abt::Constant>(resultType, resultValue));
            }
            break;
        }

        case abt::Operations::Or: {
            // Nothing and short-circuiting semantics of the 'or' operation in SBE allow us to
            // interrogate 'lhs' only. The 'rhs' can be removed only if it is 'false'.
            if (auto lhsConst = lhs.cast<abt::Constant>(); lhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                if (lhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(lhsValue)) {
                    // false || rhs -> rhs
                    swapAndUpdate(n, std::exchange(rhs, abt::make<abt::Blackhole>()));
                } else if (lhsTag == sbe::value::TypeTags::Boolean &&
                           sbe::value::bitcastTo<bool>(lhsValue)) {
                    // true || rhs -> true
                    swapAndUpdate(n, abt::Constant::boolean(true));
                }
            } else if (auto rhsConst = rhs.cast<abt::Constant>(); rhsConst) {
                auto [rhsTag, rhsValue] = rhsConst->get();
                if (rhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(rhsValue)) {
                    // lhs || false -> lhs
                    swapAndUpdate(n, std::exchange(lhs, abt::make<abt::Blackhole>()));
                }
            }
            break;
        }

        case abt::Operations::And: {
            // Nothing and short-circuiting semantics of the 'and' operation in SBE allow us to
            // interrogate 'lhs' only. The 'rhs' can be removed only if it is 'true'.
            if (auto lhsConst = lhs.cast<abt::Constant>(); lhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                if (lhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(lhsValue)) {
                    // false && rhs -> false
                    swapAndUpdate(n, abt::Constant::boolean(false));
                } else if (lhsTag == sbe::value::TypeTags::Boolean &&
                           sbe::value::bitcastTo<bool>(lhsValue)) {
                    // true && rhs -> rhs
                    swapAndUpdate(n, std::exchange(rhs, abt::make<abt::Blackhole>()));
                }
            } else if (auto rhsConst = rhs.cast<abt::Constant>(); rhsConst) {
                auto [rhsTag, rhsValue] = rhsConst->get();
                if (rhsTag == sbe::value::TypeTags::Boolean &&
                    sbe::value::bitcastTo<bool>(rhsValue)) {
                    // lhs && true -> lhs
                    swapAndUpdate(n, std::exchange(lhs, abt::make<abt::Blackhole>()));
                }
            }
            break;
        }

        case abt::Operations::Eq:
        case abt::Operations::Neq:
        case abt::Operations::Lt:
        case abt::Operations::Lte:
        case abt::Operations::Gt:
        case abt::Operations::Gte:
        case abt::Operations::Cmp3w: {
            const auto lhsConst = lhs.cast<abt::Constant>();
            const auto rhsConst = rhs.cast<abt::Constant>();
            if (lhsConst && rhsConst) {
                // Call the appropriate genericXXX() to get the result of the comparison op.
                const auto [lhsTag, lhsVal] = lhsConst->get();
                const auto [rhsTag, rhsVal] = rhsConst->get();
                auto compareFunc = [&] {
                    if (op.op() == abt::Operations::Eq) {
                        return &sbe::value::genericEq;
                    } else if (op.op() == abt::Operations::Neq) {
                        return &sbe::value::genericNeq;
                    } else if (op.op() == abt::Operations::Lt) {
                        return &sbe::value::genericLt;
                    } else if (op.op() == abt::Operations::Lte) {
                        return &sbe::value::genericLte;
                    } else if (op.op() == abt::Operations::Gt) {
                        return &sbe::value::genericGt;
                    } else if (op.op() == abt::Operations::Gte) {
                        return &sbe::value::genericGte;
                    } else if (op.op() == abt::Operations::Cmp3w) {
                        return &sbe::value::compare3way;
                    } else {
                        MONGO_UNREACHABLE;
                    }
                }();

                const auto [tag, val] = compareFunc(lhsTag, lhsVal, rhsTag, rhsVal, _collator);

                // Replace the comparison expression with the result of the comparison op.
                swapAndUpdate(n, abt::make<abt::Constant>(tag, val));
            } else if ((lhsConst && lhsConst->isNothing()) || (rhsConst && rhsConst->isNothing())) {
                // If either arg is Nothing, the comparison op will produce Nothing.
                swapAndUpdate(n, abt::Constant::nothing());
            }
            break;
        }

        default:
            // Not implemented.
            break;
    }
}

void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::NaryOp& op,
                                    std::vector<abt::ABT>& args) {
    switch (op.op()) {
        case abt::Operations::And:
        case abt::Operations::Or: {
            // Truncate the list at the first constant value that causes the operation to
            // short-circuit ('false' for And, 'true' for Or). Remove all items that evaluate to the
            // opposite constant, as they would be ignored.
            bool shortCircuitValue = op.op() == abt::Operations::And ? false : true;
            for (auto it = args.begin(); it < args.end();) {
                abt::ABT& arg = *it;
                if (auto argConst = arg.cast<abt::Constant>(); argConst) {
                    auto [argTag, argValue] = argConst->get();
                    if (argTag == sbe::value::TypeTags::Boolean) {
                        if (sbe::value::bitcastTo<bool>(argValue) == shortCircuitValue) {
                            // Truncate the argument list after the short-circuit value, unless it's
                            // already the last value and we are not modifying anything.
                            if (it + 1 != args.end()) {
                                args.erase(it + 1, args.end());
                                _changed = true;
                                break;
                            }
                        } else {
                            // Remove argument.
                            it = args.erase(it);
                            _changed = true;
                            continue;
                        }
                    }
                }
                it++;
            }
            if (args.empty()) {
                // if we are left with no arguments, replace the entire node with the
                // non-short-circuit value.
                swapAndUpdate(n, abt::Constant::boolean(!shortCircuitValue));
            } else if (args.size() == 1) {
                // if we are left with just one argument, replace the entire node with that value.
                swapAndUpdate(n, std::exchange(args[0], abt::make<abt::Blackhole>()));
            }
            break;
        }
        case abt::Operations::Add:
        case abt::Operations::Mult: {
            auto it = args.begin();
            if (it->cast<abt::Constant>()) {
                it++;
                for (; it < args.end(); it++) {
                    abt::ABT& rhs = *it;
                    auto rhsConst = rhs.cast<abt::Constant>();
                    if (!rhsConst) {
                        break;
                    }
                    abt::ABT& lhs = *(it - 1);
                    auto lhsConst = lhs.cast<abt::Constant>();

                    auto [lhsTag, lhsValue] = lhsConst->get();
                    auto [rhsTag, rhsValue] = rhsConst->get();

                    auto performOp = [&](sbe::value::TypeTags lhsTag,
                                         sbe::value::Value lhsValue,
                                         sbe::value::TypeTags rhsTag,
                                         sbe::value::Value rhsValue) {
                        switch (op.op()) {
                            case abt::Operations::Add:
                                return sbe::value::genericAdd(lhsTag, lhsValue, rhsTag, rhsValue);
                            case abt::Operations::Mult:
                                return sbe::value::genericMul(lhsTag, lhsValue, rhsTag, rhsValue);
                            default:
                                MONGO_UNREACHABLE;
                        }
                    };

                    auto [_, resultType, resultValue] =
                        performOp(lhsTag, lhsValue, rhsTag, rhsValue);
                    swapAndUpdate(rhs, abt::make<abt::Constant>(resultType, resultValue));
                }
                args.erase(args.begin(), it - 1);
            }
            invariant(args.size() > 0);
            if (args.size() == 1) {
                swapAndUpdate(n, std::exchange(args[0], abt::make<abt::Blackhole>()));
            }
            break;
        }
        default:
            // Not implemented.
            break;
    }
}

void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::FunctionCall& op,
                                    std::vector<abt::ABT>& args) {
    if (args.size() == 1 && args[0].is<abt::Constant>()) {
        // We can simplify exists(constant) to true if the said constant is not Nothing.
        if (op.name() == "exists"s) {
            auto [tag, val] = args[0].cast<abt::Constant>()->get();
            swapAndUpdate(n, abt::Constant::boolean(tag != sbe::value::TypeTags::Nothing));
        }

        // We can simplify coerceToBool(constant).
        if (op.name() == "coerceToBool"s) {
            auto [tag, val] = args[0].cast<abt::Constant>()->get();
            auto [resultTag, resultVal] = sbe::value::coerceToBool(tag, val);
            swapAndUpdate(n, abt::make<abt::Constant>(resultTag, resultVal));
        }

        // We can simplify isTimeUnit(constant).
        if (op.name() == "isTimeUnit"s) {
            auto [tag, val] = args[0].cast<abt::Constant>()->get();
            if (sbe::value::isString(tag)) {
                swapAndUpdate(
                    n,
                    abt::Constant::boolean(isValidTimeUnit(sbe::value::getStringView(tag, val))));
            } else {
                swapAndUpdate(n, abt::Constant::nothing());
            }
        }
    }

    // We can simplify typeMatch(constant, constantMask).
    if (args.size() == 2 && args[0].is<abt::Constant>() && args[1].is<abt::Constant>()) {
        if (op.name() == "typeMatch"s) {
            auto [tag, val] = args[0].cast<abt::Constant>()->get();
            if (tag == sbe::value::TypeTags::Nothing) {
                swapAndUpdate(n, abt::Constant::nothing());
            } else {
                auto [tagMask, valMask] = args[1].cast<abt::Constant>()->get();
                if (tagMask == sbe::value::TypeTags::NumberInt32) {
                    auto bsonMask = static_cast<uint32_t>(sbe::value::bitcastTo<int32_t>(valMask));
                    swapAndUpdate(n,
                                  abt::Constant::boolean((getBSONTypeMask(tag) & bsonMask) != 0));
                }
            }
        }

        // We can simplify convert(constant).
        if (op.name() == "convert"s) {
            auto [tag, val] = args[0].cast<abt::Constant>()->get();
            if (tag == sbe::value::TypeTags::Nothing) {
                swapAndUpdate(n, abt::Constant::nothing());
            } else {
                auto [tagRhs, valRhs] = args[1].cast<abt::Constant>()->get();
                if (tagRhs == sbe::value::TypeTags::NumberInt32) {
                    sbe::value::TypeTags targetTypeTag =
                        (sbe::value::TypeTags)sbe::value::bitcastTo<int32_t>(valRhs);
                    auto [_, convertedTag, convertedVal] =
                        sbe::value::genericNumConvert(tag, val, targetTypeTag);
                    swapAndUpdate(n, abt::make<abt::Constant>(convertedTag, convertedVal));
                }
            }
        }
    }

    if (op.name() == "newArray"s) {
        bool allConstants = true;
        for (const abt::ABT& arg : op.nodes()) {
            if (!arg.is<abt::Constant>()) {
                allConstants = false;
                break;
            }
        }

        if (allConstants) {
            // All arguments are constants. Replace with an array constant.

            sbe::value::Array array;
            for (const abt::ABT& arg : op.nodes()) {
                auto [tag, val] = arg.cast<abt::Constant>()->get();
                // Copy the value before inserting into the array.
                auto [tagCopy, valCopy] = sbe::value::copyValue(tag, val);
                array.push_back(tagCopy, valCopy);
            }

            auto [tag, val] = sbe::value::makeCopyArray(array);
            swapAndUpdate(n, abt::make<abt::Constant>(tag, val));
        }
    }
    if (op.name() == "isInList"s) {
        // If the child node is a Constant, check if the type is inList, then directly set to
        // true/false.
        if (args.size() == 1 && args[0].is<abt::Constant>()) {
            const auto tag = args[0].cast<abt::Constant>()->get().first;
            swapAndUpdate(n, abt::Constant::boolean(tag == sbe::value::TypeTags::inList));
        }
    }
    if (op.name() == "setIsSubset"s || op.name() == "setDifference"s || op.name() == "setEquals"s) {
        // Convert any argument of type Array to an ArraySet.
        for (size_t idx = 0; idx < args.size(); idx++) {
            if (args[idx].is<abt::Constant>()) {
                auto [tag, val] = args[idx].cast<abt::Constant>()->get();
                switch (tag) {
                    case sbe::value::TypeTags::Array:
                    case sbe::value::TypeTags::ArrayMultiSet:
                    case sbe::value::TypeTags::bsonArray: {
                        auto [setTag, setVal] = sbe::value::makeNewArraySet(tag, val, nullptr);
                        swapAndUpdate(args[idx], abt::make<abt::Constant>(setTag, setVal));
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
}

void ExpressionConstEval::transport(
    abt::ABT& n, const abt::If& op, abt::ABT& cond, abt::ABT& thenBranch, abt::ABT& elseBranch) {
    // If the condition is a boolean constant we can simplify.
    if (auto condConst = cond.cast<abt::Constant>(); condConst) {
        auto [condTag, condValue] = condConst->get();
        if (condTag == sbe::value::TypeTags::Boolean && sbe::value::bitcastTo<bool>(condValue)) {
            // if true -> thenBranch
            swapAndUpdate(n, std::exchange(thenBranch, abt::make<abt::Blackhole>()));
        } else if (condTag == sbe::value::TypeTags::Boolean &&
                   !sbe::value::bitcastTo<bool>(condValue)) {
            // if false -> elseBranch
            swapAndUpdate(n, std::exchange(elseBranch, abt::make<abt::Blackhole>()));
        } else if (condTag == sbe::value::TypeTags::Nothing) {
            // if Nothing then x else y -> Nothing
            swapAndUpdate(n, abt::Constant::nothing());
        }
    } else if (auto condNot = cond.cast<abt::UnaryOp>();
               condNot && condNot->op() == abt::Operations::Not) {
        // If the condition is a Not we can remove it and swap the branches.
        swapAndUpdate(cond, std::exchange(condNot->get<0>(), abt::make<abt::Blackhole>()));
        std::swap(thenBranch, elseBranch);
    } else if (auto funct = cond.cast<abt::FunctionCall>(); funct && funct->name() == "exists"s &&
               funct->nodes().size() == 1 && funct->nodes()[0] == thenBranch &&
               elseBranch.is<abt::Constant>()) {
        // If the condition is an "exists" on an expression, the thenBranch is the same
        // expression and the elseBranch is a constant, the node is actually a FillEmpty. Note
        // that this is not true if the replacement value is an expression that can have side
        // effects, because FillEmpty has to evaluate both operands before deciding which one to
        // return: keeping the if(exists(..)) allows not to evaluate the elseBranch when the
        // condition returns true.
        swapAndUpdate(
            n,
            abt::make<abt::BinaryOp>(abt::Operations::FillEmpty,
                                     std::exchange(thenBranch, abt::make<abt::Blackhole>()),
                                     std::exchange(elseBranch, abt::make<abt::Blackhole>())));
    } else if (auto thenConst = thenBranch.cast<abt::Constant>(),
               elseConst = elseBranch.cast<abt::Constant>();
               thenConst && elseConst) {
        // If both branches are boolean constants then we can simplify.
        if (auto [thenTag, thenValue] = thenConst->get();
            thenTag == sbe::value::TypeTags::Boolean) {
            const bool v1 = sbe::value::bitcastTo<bool>(thenValue);
            if (auto [elseTag, elseValue] = elseConst->get();
                elseTag == sbe::value::TypeTags::Boolean) {
                const bool v2 = sbe::value::bitcastTo<bool>(elseValue);
                if (v1 && !v2) {
                    // if (x) then true else false -> (x).
                    swapAndUpdate(n, std::exchange(cond, abt::make<abt::Blackhole>()));
                } else if (!v1 && v2) {
                    // If (x) then false else true -> !(x).
                    swapAndUpdate(
                        n,
                        abt::make<abt::UnaryOp>(abt::Operations::Not,
                                                std::exchange(cond, abt::make<abt::Blackhole>())));
                }
                // "if (x) then true else true" and "if (x) then false else false" cannot be
                // folded because we need to return Nothing if non-const 'x' is Nothing.
            }
        }
    }
}

void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::Switch& op,
                                    std::vector<abt::ABT>& args) {
    // If the condition is a boolean constant we can remove it from the branches.
    for (size_t i = 0; i < args.size() - 1;) {
        abt::ABT& cond = args[i];
        if (auto condConst = cond.cast<abt::Constant>(); condConst) {
            auto [condTag, condValue] = condConst->get();
            if (condTag == sbe::value::TypeTags::Boolean &&
                sbe::value::bitcastTo<bool>(condValue)) {
                // if true -> remove this branch and the remaining ones, promote the
                // "thenBranch" to become the "defaultExpr".
                args.erase(std::next(args.begin(), i));
                args.erase(std::next(args.begin(), i + 1), args.end());
                _changed = true;
                continue;
            } else if (condTag == sbe::value::TypeTags::Boolean &&
                       !sbe::value::bitcastTo<bool>(condValue)) {
                // if false -> remove branch.
                args.erase(std::next(args.begin(), i), std::next(args.begin(), i + 2));
                _changed = true;
                continue;
            }
        }
        i += 2;
    }
    // If we are left with no branches, replace the entire Switch with the default expression.
    if (op.nodes().size() == 1) {
        swapAndUpdate(n, std::exchange(args[0], abt::make<abt::Blackhole>()));
    } else if (op.getNumBranches() == 1) {
        // Convert Switch into If if there is a single branch, then constant fold it.
        auto ifNode = abt::make<abt::If>(std::exchange(args[0], abt::make<abt::Blackhole>()),
                                         std::exchange(args[1], abt::make<abt::Blackhole>()),
                                         std::exchange(args[2], abt::make<abt::Blackhole>()));
        transport(ifNode,
                  *ifNode.cast<abt::If>(),
                  ifNode.cast<abt::If>()->getCondChild(),
                  ifNode.cast<abt::If>()->getThenChild(),
                  ifNode.cast<abt::If>()->getElseChild());

        swapAndUpdate(n, ifNode);
    } else {
        // Check if the last condition contains a Not that can be removed by swapping the branches.
        auto& cond = args[args.size() - 3];
        auto& thenBranch = args[args.size() - 2];
        auto& elseBranch = args[args.size() - 1];
        if (auto condNot = cond.cast<abt::UnaryOp>();
            condNot && condNot->op() == abt::Operations::Not) {
            // If the condition is a Not we can remove it and swap the branches.
            swapAndUpdate(cond, std::exchange(condNot->get<0>(), abt::make<abt::Blackhole>()));
            std::swap(thenBranch, elseBranch);
        }
    }
}

void ExpressionConstEval::prepare(abt::ABT& n, const abt::LambdaAbstraction& lam) {
    ++_inCostlyCtx;
    _variableDefinitions.emplace(lam.varName(),
                                 abt::Definition{n.ref(), abt::ABT::reference_type{}});
}

void ExpressionConstEval::transport(abt::ABT&, const abt::LambdaAbstraction& lam, abt::ABT&) {
    --_inCostlyCtx;
    _variableDefinitions.erase(lam.varName());
}

void ExpressionConstEval::prepare(abt::ABT&, const abt::References& refs) {
    // It is structurally impossible to nest abt::References nodes.
    invariant(!_inRefBlock);
    _inRefBlock = true;
}
void ExpressionConstEval::transport(abt::ABT& n,
                                    const abt::References& op,
                                    std::vector<abt::ABT>&) {
    invariant(_inRefBlock);
    _inRefBlock = false;
}

void ExpressionConstEval::swapAndUpdate(abt::ABT& n, abt::ABT newN) {
    // Record the mapping from the old to the new.
    invariant(_staleDefs.count(n.ref()) == 0);
    invariant(_staleDefs.count(newN.ref()) == 0);

    _staleDefs[n.ref()] = newN.ref();

    // Do the swap.
    std::swap(n, newN);

    // newN now contains the old abt::ABT
    _staleABTs.emplace_back(std::move(newN));

    _changed = true;
}
}  // namespace mongo::stage_builder
