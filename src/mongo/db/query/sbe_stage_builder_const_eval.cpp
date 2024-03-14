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

#include "mongo/db/query/sbe_stage_builder_const_eval.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/util/assert_util.h"

namespace mongo::stage_builder {
bool ExpressionConstEval::optimize(optimizer::ABT& n) {
    invariant(_letRefs.empty());
    invariant(_singleRef.empty());
    invariant(!_inRefBlock);
    invariant(_inCostlyCtx == 0);
    invariant(_staleDefs.empty());
    invariant(_staleABTs.empty());

    _changed = false;

    // We run the transport<true> that will pass the reference to optimizer::ABT to specific
    // transport functions. The reference serves as a conceptual 'this' pointer allowing the
    // transport function to change the node itself.
    optimizer::algebra::transport<true>(n, *this);
    invariant(_letRefs.empty());

    while (_changed) {
        if (_singleRef.empty()) {
            break;
        }
        _changed = false;
        optimizer::algebra::transport<true>(n, *this);
    }

    // TODO: should we be clearing here?
    _singleRef.clear();

    _staleDefs.clear();
    _staleABTs.clear();
    return _changed;
}

void ExpressionConstEval::transport(optimizer::ABT& n, const optimizer::Variable& var) {
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

            if (auto constant = def.definition.cast<optimizer::Constant>();
                constant && !_inRefBlock) {
                // If we find the definition and it is a simple constant then substitute the
                // variable.
                swapAndUpdate(n, def.definition.copy());
            } else if (auto variable = def.definition.cast<optimizer::Variable>();
                       variable && !_inRefBlock) {
                swapAndUpdate(n, def.definition.copy());
            } else if (_singleRef.erase(&var)) {
                swapAndUpdate(n, def.definition.copy());
            } else if (auto let = def.definedBy.cast<optimizer::Let>(); let) {
                invariant(_letRefs.count(let));
                _letRefs[let].emplace_back(&var);
            }
        }
    }
}

void ExpressionConstEval::prepare(optimizer::ABT& n, const optimizer::Let& let) {
    _letRefs[&let] = {};
    _variableDefinitions.emplace(let.varName(), optimizer::Definition{n.ref(), let.bind().ref()});
}

void ExpressionConstEval::transport(optimizer::ABT& n,
                                    const optimizer::Let& let,
                                    optimizer::ABT& bind,
                                    optimizer::ABT& in) {
    auto& letRefs = _letRefs[&let];
    if (letRefs.size() == 0) {
        // The bind expressions has not been referenced so it is dead code and the whole let
        // expression can be removed; i.e. we implement a following rewrite:
        //
        // n == let var=<bind expr> in <in expr>
        //
        //     v
        //
        // n == <in expr>

        // We don't want to optimizer::make a copy of 'in' as it may be arbitrarily large. Also, we
        // cannot move it out as it is part of the Let object and we do not want to invalidate any
        // assumptions the Let may have about its structure. Hence we swap it for the "special"
        // Blackhole object. The Blackhole does nothing, it just plugs the hole left in the 'in'
        // place.
        auto result = std::exchange(in, optimizer::make<optimizer::Blackhole>());

        // Swap the current node (n) for the result.
        swapAndUpdate(n, std::move(result));
    } else if (letRefs.size() == 1) {
        // The bind expression has been referenced exactly once so schedule it for inlining.
        _singleRef.emplace(letRefs.front());
        _changed = true;
    }
    _letRefs.erase(&let);
    _variableDefinitions.erase(let.varName());
}

void ExpressionConstEval::transport(optimizer::ABT& n,
                                    const optimizer::LambdaApplication& app,
                                    optimizer::ABT& lam,
                                    optimizer::ABT& arg) {
    // If the 'lam' expression is optimizer::LambdaAbstraction then we can do the inplace beta
    // reduction.
    // TODO - missing alpha conversion so for now assume globally unique names.
    if (auto lambda = lam.cast<optimizer::LambdaAbstraction>(); lambda) {
        auto result = optimizer::make<optimizer::Let>(
            lambda->varName(),
            std::exchange(arg, optimizer::make<optimizer::Blackhole>()),
            std::exchange(lambda->getBody(), optimizer::make<optimizer::Blackhole>()));

        swapAndUpdate(n, std::move(result));
    }
}

void ExpressionConstEval::transport(optimizer::ABT& n,
                                    const optimizer::UnaryOp& op,
                                    optimizer::ABT& child) {
    switch (op.op()) {
        case optimizer::Operations::Not: {
            if (const auto childConst = child.cast<optimizer::Constant>();
                childConst && childConst->isValueBool()) {
                swapAndUpdate(n, optimizer::Constant::boolean(!childConst->getValueBool()));
            }
            break;
        }

        default:
            break;
    }
}

// Specific transport for binary operation
// The const correctness is probably wrong (as const optimizer::ABT& lhs, const optimizer::ABT& rhs
// does not work for some reason but we can fix it later).
void ExpressionConstEval::transport(optimizer::ABT& n,
                                    const optimizer::BinaryOp& op,
                                    optimizer::ABT& lhs,
                                    optimizer::ABT& rhs) {

    switch (op.op()) {
        case optimizer::Operations::Add: {
            // Let say we want to recognize ConstLhs + ConstRhs and replace it with the result of
            // addition.
            auto lhsConst = lhs.cast<optimizer::Constant>();
            auto rhsConst = rhs.cast<optimizer::Constant>();
            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericAdd(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, optimizer::make<optimizer::Constant>(resultType, resultValue));
            }
            break;
        }

        case optimizer::Operations::Sub: {
            // Let say we want to recognize ConstLhs - ConstRhs and replace it with the result of
            // subtraction.
            auto lhsConst = lhs.cast<optimizer::Constant>();
            auto rhsConst = rhs.cast<optimizer::Constant>();

            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericSub(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, optimizer::make<optimizer::Constant>(resultType, resultValue));
            }
            break;
        }

        case optimizer::Operations::Mult: {
            // Let say we want to recognize ConstLhs * ConstRhs and replace it with the result of
            // multiplication.
            auto lhsConst = lhs.cast<optimizer::Constant>();
            auto rhsConst = rhs.cast<optimizer::Constant>();

            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericMul(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, optimizer::make<optimizer::Constant>(resultType, resultValue));
            }
            break;
        }

        case optimizer::Operations::Or: {
            // Nothing and short-circuiting semantics of the 'or' operation in SBE allow us to
            // interrogate 'lhs' only. The 'rhs' can be removed only if it is 'false'.
            if (auto lhsConst = lhs.cast<optimizer::Constant>(); lhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                if (lhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(lhsValue)) {
                    // false || rhs -> rhs
                    swapAndUpdate(n, std::exchange(rhs, optimizer::make<optimizer::Blackhole>()));
                } else if (lhsTag == sbe::value::TypeTags::Boolean &&
                           sbe::value::bitcastTo<bool>(lhsValue)) {
                    // true || rhs -> true
                    swapAndUpdate(n, optimizer::Constant::boolean(true));
                }
            } else if (auto rhsConst = rhs.cast<optimizer::Constant>(); rhsConst) {
                auto [rhsTag, rhsValue] = rhsConst->get();
                if (rhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(rhsValue)) {
                    // lhs || false -> lhs
                    swapAndUpdate(n, std::exchange(lhs, optimizer::make<optimizer::Blackhole>()));
                }
            }
            break;
        }

        case optimizer::Operations::And: {
            // Nothing and short-circuiting semantics of the 'and' operation in SBE allow us to
            // interrogate 'lhs' only. The 'rhs' can be removed only if it is 'true'.
            if (auto lhsConst = lhs.cast<optimizer::Constant>(); lhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                if (lhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(lhsValue)) {
                    // false && rhs -> false
                    swapAndUpdate(n, optimizer::Constant::boolean(false));
                } else if (lhsTag == sbe::value::TypeTags::Boolean &&
                           sbe::value::bitcastTo<bool>(lhsValue)) {
                    // true && rhs -> rhs
                    swapAndUpdate(n, std::exchange(rhs, optimizer::make<optimizer::Blackhole>()));
                }
            } else if (auto rhsConst = rhs.cast<optimizer::Constant>(); rhsConst) {
                auto [rhsTag, rhsValue] = rhsConst->get();
                if (rhsTag == sbe::value::TypeTags::Boolean &&
                    sbe::value::bitcastTo<bool>(rhsValue)) {
                    // lhs && true -> lhs
                    swapAndUpdate(n, std::exchange(lhs, optimizer::make<optimizer::Blackhole>()));
                }
            }
            break;
        }

        case optimizer::Operations::Eq:
        case optimizer::Operations::Neq:
        case optimizer::Operations::Lt:
        case optimizer::Operations::Lte:
        case optimizer::Operations::Gt:
        case optimizer::Operations::Gte:
        case optimizer::Operations::Cmp3w: {
            const auto lhsConst = lhs.cast<optimizer::Constant>();
            const auto rhsConst = rhs.cast<optimizer::Constant>();
            if (lhsConst && rhsConst) {
                // Call the appropriate genericXXX() to get the result of the comparison op.
                const auto [lhsTag, lhsVal] = lhsConst->get();
                const auto [rhsTag, rhsVal] = rhsConst->get();
                auto compareFunc = [&] {
                    if (op.op() == optimizer::Operations::Eq) {
                        return &sbe::value::genericEq;
                    } else if (op.op() == optimizer::Operations::Neq) {
                        return &sbe::value::genericNeq;
                    } else if (op.op() == optimizer::Operations::Lt) {
                        return &sbe::value::genericLt;
                    } else if (op.op() == optimizer::Operations::Lte) {
                        return &sbe::value::genericLte;
                    } else if (op.op() == optimizer::Operations::Gt) {
                        return &sbe::value::genericGt;
                    } else if (op.op() == optimizer::Operations::Gte) {
                        return &sbe::value::genericGte;
                    } else if (op.op() == optimizer::Operations::Cmp3w) {
                        return &sbe::value::compare3way;
                    } else {
                        MONGO_UNREACHABLE;
                    }
                }();

                const auto [tag, val] = compareFunc(lhsTag, lhsVal, rhsTag, rhsVal, _collator);

                // Replace the comparison expression with the result of the comparison op.
                swapAndUpdate(n, optimizer::make<optimizer::Constant>(tag, val));
            } else if ((lhsConst && lhsConst->isNothing()) || (rhsConst && rhsConst->isNothing())) {
                // If either arg is Nothing, the comparison op will produce Nothing.
                swapAndUpdate(n, optimizer::Constant::nothing());
            }
            break;
        }

        default:
            // Not implemented.
            break;
    }
}

void ExpressionConstEval::transport(optimizer::ABT& n,
                                    const optimizer::FunctionCall& op,
                                    std::vector<optimizer::ABT>& args) {
    if (args.size() == 1 && args[0].is<optimizer::Constant>()) {
        // We can simplify exists(constant) to true if the said constant is not Nothing.
        if (op.name() == "exists"s) {
            auto [tag, val] = args[0].cast<optimizer::Constant>()->get();
            swapAndUpdate(n, optimizer::Constant::boolean(tag != sbe::value::TypeTags::Nothing));
        }

        // We can simplify coerceToBool(constant).
        if (op.name() == "coerceToBool"s) {
            auto [tag, val] = args[0].cast<optimizer::Constant>()->get();
            auto [resultTag, resultVal] = sbe::value::coerceToBool(tag, val);
            swapAndUpdate(n, optimizer::make<optimizer::Constant>(resultTag, resultVal));
        }

        // We can simplify isTimeUnit(constant).
        if (op.name() == "isTimeUnit"s) {
            auto [tag, val] = args[0].cast<optimizer::Constant>()->get();
            if (sbe::value::isString(tag)) {
                swapAndUpdate(n,
                              optimizer::Constant::boolean(
                                  isValidTimeUnit(sbe::value::getStringView(tag, val))));
            } else {
                swapAndUpdate(n, optimizer::Constant::nothing());
            }
        }
    }

    // We can simplify typeMatch(constant, constantMask).
    if (args.size() == 2 && args[0].is<optimizer::Constant>() &&
        args[1].is<optimizer::Constant>()) {
        if (op.name() == "typeMatch"s) {
            auto [tag, val] = args[0].cast<optimizer::Constant>()->get();
            if (tag == sbe::value::TypeTags::Nothing) {
                swapAndUpdate(n, optimizer::Constant::nothing());
            } else {
                auto [tagMask, valMask] = args[1].cast<optimizer::Constant>()->get();
                if (tagMask == sbe::value::TypeTags::NumberInt32) {
                    auto bsonMask = static_cast<uint32_t>(sbe::value::bitcastTo<int32_t>(valMask));
                    swapAndUpdate(
                        n, optimizer::Constant::boolean((getBSONTypeMask(tag) & bsonMask) != 0));
                }
            }
        }

        // We can simplify convert(constant).
        if (op.name() == "convert"s) {
            auto [tag, val] = args[0].cast<optimizer::Constant>()->get();
            if (tag == sbe::value::TypeTags::Nothing) {
                swapAndUpdate(n, optimizer::Constant::nothing());
            } else {
                auto [tagRhs, valRhs] = args[1].cast<optimizer::Constant>()->get();
                if (tagRhs == sbe::value::TypeTags::NumberInt32) {
                    sbe::value::TypeTags targetTypeTag =
                        (sbe::value::TypeTags)sbe::value::bitcastTo<int32_t>(valRhs);
                    auto [_, convertedTag, convertedVal] =
                        sbe::value::genericNumConvert(tag, val, targetTypeTag);
                    swapAndUpdate(n,
                                  optimizer::make<optimizer::Constant>(convertedTag, convertedVal));
                }
            }
        }
    }

    if (op.name() == "newArray"s) {
        bool allConstants = true;
        for (const optimizer::ABT& arg : op.nodes()) {
            if (!arg.is<optimizer::Constant>()) {
                allConstants = false;
                break;
            }
        }

        if (allConstants) {
            // All arguments are constants. Replace with an array constant.

            sbe::value::Array array;
            for (const optimizer::ABT& arg : op.nodes()) {
                auto [tag, val] = arg.cast<optimizer::Constant>()->get();
                // Copy the value before inserting into the array.
                auto [tagCopy, valCopy] = sbe::value::copyValue(tag, val);
                array.push_back(tagCopy, valCopy);
            }

            auto [tag, val] = sbe::value::makeCopyArray(array);
            swapAndUpdate(n, optimizer::make<optimizer::Constant>(tag, val));
        }
    }
}

void ExpressionConstEval::transport(optimizer::ABT& n,
                                    const optimizer::If& op,
                                    optimizer::ABT& cond,
                                    optimizer::ABT& thenBranch,
                                    optimizer::ABT& elseBranch) {
    // If the condition is a boolean constant we can simplify.
    if (auto condConst = cond.cast<optimizer::Constant>(); condConst) {
        auto [condTag, condValue] = condConst->get();
        if (condTag == sbe::value::TypeTags::Boolean && sbe::value::bitcastTo<bool>(condValue)) {
            // if true -> thenBranch
            swapAndUpdate(n, std::exchange(thenBranch, optimizer::make<optimizer::Blackhole>()));
        } else if (condTag == sbe::value::TypeTags::Boolean &&
                   !sbe::value::bitcastTo<bool>(condValue)) {
            // if false -> elseBranch
            swapAndUpdate(n, std::exchange(elseBranch, optimizer::make<optimizer::Blackhole>()));
        }
    } else if (auto condNot = cond.cast<optimizer::UnaryOp>();
               condNot && condNot->op() == optimizer::Operations::Not) {
        // If the condition is a Not we can remove it and swap the branches.
        swapAndUpdate(cond,
                      std::exchange(condNot->get<0>(), optimizer::make<optimizer::Blackhole>()));
        std::swap(thenBranch, elseBranch);
    } else if (auto funct = cond.cast<optimizer::FunctionCall>(); funct &&
               funct->name() == "exists" && funct->nodes().size() == 1 &&
               funct->nodes()[0] == thenBranch && elseBranch.is<optimizer::Constant>()) {
        // If the condition is an "exists" on an expression, the thenBranch is the same expression
        // and the elseBranch is a constant, the node is actually a FillEmpty.
        // Note that this is not true if the replacement value is an expression that can have side
        // effects, because FillEmpty has to evaluate both operands before deciding which one to
        // return: keeping the if(exists(..)) allows not to evaluate the elseBranch when the
        // condition returns true.
        swapAndUpdate(n,
                      optimizer::make<optimizer::BinaryOp>(
                          optimizer::Operations::FillEmpty,
                          std::exchange(thenBranch, optimizer::make<optimizer::Blackhole>()),
                          std::exchange(elseBranch, optimizer::make<optimizer::Blackhole>())));
    }
}

void ExpressionConstEval::prepare(optimizer::ABT& n, const optimizer::LambdaAbstraction& lam) {
    ++_inCostlyCtx;
    _variableDefinitions.emplace(lam.varName(),
                                 optimizer::Definition{n.ref(), optimizer::ABT::reference_type{}});
}

void ExpressionConstEval::transport(optimizer::ABT&,
                                    const optimizer::LambdaAbstraction& lam,
                                    optimizer::ABT&) {
    --_inCostlyCtx;
    _variableDefinitions.erase(lam.varName());
}

void ExpressionConstEval::prepare(optimizer::ABT&, const optimizer::References& refs) {
    // It is structurally impossible to nest optimizer::References nodes.
    invariant(!_inRefBlock);
    _inRefBlock = true;
}
void ExpressionConstEval::transport(optimizer::ABT& n,
                                    const optimizer::References& op,
                                    std::vector<optimizer::ABT>&) {
    invariant(_inRefBlock);
    _inRefBlock = false;
}

void ExpressionConstEval::swapAndUpdate(optimizer::ABT& n, optimizer::ABT newN) {
    // Record the mapping from the old to the new.
    invariant(_staleDefs.count(n.ref()) == 0);
    invariant(_staleDefs.count(newN.ref()) == 0);

    _staleDefs[n.ref()] = newN.ref();

    // Do the swap.
    std::swap(n, newN);

    // newN now contains the old optimizer::ABT
    _staleABTs.emplace_back(std::move(newN));

    _changed = true;
}
}  // namespace mongo::stage_builder
