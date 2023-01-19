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
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/optimizer/utils/utils.h"

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
        _env.rebuild(n);

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
    auto def = _env.getDefinition(var);

    if (!def.definition.empty()) {
        // See if we have already manipulated this definition and if so then use the newer version.
        if (auto it = _staleDefs.find(def.definition); it != _staleDefs.end()) {
            def.definition = it->second;
        }
        if (auto it = _staleDefs.find(def.definedBy); it != _staleDefs.end()) {
            def.definedBy = it->second;
        }

        if (auto constant = def.definition.cast<optimizer::Constant>(); constant && !_inRefBlock) {
            // If we find the definition and it is a simple constant then substitute the variable.
            swapAndUpdate(n, def.definition);
        } else if (auto variable = def.definition.cast<optimizer::Variable>();
                   variable && !_inRefBlock) {
            swapAndUpdate(n, def.definition);
        } else if (_singleRef.erase(&var)) {
            swapAndUpdate(n, def.definition);
        } else if (auto let = def.definedBy.cast<optimizer::Let>(); let) {
            invariant(_letRefs.count(let));
            _letRefs[let].emplace_back(&var);
        }
    }
}

void ExpressionConstEval::prepare(optimizer::ABT&, const optimizer::Let& let) {
    _letRefs[&let] = {};
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
            // interrogate 'lhs' only.
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
            }
            break;
        }

        case optimizer::Operations::And: {
            // Nothing and short-circuiting semantics of the 'and' operation in SBE allow us to
            // interrogate 'lhs' only.
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
            }
            break;
        }

        case optimizer::Operations::Eq: {
            if (lhs == rhs) {
                // If the subtrees are equal, we can conclude that their result is equal because we
                // have only pure functions.
                swapAndUpdate(n, optimizer::Constant::boolean(true));
            } else {
                const auto lhsConst = lhs.cast<optimizer::Constant>();
                const auto rhsConst = rhs.cast<optimizer::Constant>();
                if (lhsConst && rhsConst) {
                    // We have two non-equal constants, but they still may be equal from _collator's
                    // point of view.
                    if (_collator == nullptr) {
                        swapAndUpdate(n, optimizer::Constant::boolean(false));
                    } else {
                        const auto [lhsTag, lhsVal] = lhsConst->get();
                        const auto [rhsTag, rhsVal] = rhsConst->get();
                        const auto [compareTag, compareVal] =
                            sbe::value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal, _collator);
                        uassert(7291100,
                                "Invalid comparison result",
                                compareTag == sbe::value::TypeTags::NumberInt32);
                        const int32_t cmpVal = sbe::value::bitcastTo<int32_t>(compareVal);
                        swapAndUpdate(n, optimizer::Constant::boolean(cmpVal == 0));
                    }
                }
            }
            break;
        }

        case optimizer::Operations::Lt:
        case optimizer::Operations::Lte:
        case optimizer::Operations::Gt:
        case optimizer::Operations::Gte:
        case optimizer::Operations::Cmp3w: {
            const auto lhsConst = lhs.cast<optimizer::Constant>();
            const auto rhsConst = rhs.cast<optimizer::Constant>();

            if (lhsConst) {
                const auto [lhsTag, lhsVal] = lhsConst->get();

                if (rhsConst) {
                    const auto [rhsTag, rhsVal] = rhsConst->get();

                    const auto [compareTag, compareVal] =
                        sbe::value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal, _collator);
                    uassert(7291101,
                            "Invalid comparison result",
                            compareTag == sbe::value::TypeTags::NumberInt32);
                    const auto cmpVal = sbe::value::bitcastTo<int32_t>(compareVal);

                    switch (op.op()) {
                        case optimizer::Operations::Lt:
                            swapAndUpdate(n, optimizer::Constant::boolean(cmpVal < 0));
                            break;
                        case optimizer::Operations::Lte:
                            swapAndUpdate(n, optimizer::Constant::boolean(cmpVal <= 0));
                            break;
                        case optimizer::Operations::Gt:
                            swapAndUpdate(n, optimizer::Constant::boolean(cmpVal > 0));
                            break;
                        case optimizer::Operations::Gte:
                            swapAndUpdate(n, optimizer::Constant::boolean(cmpVal >= 0));
                            break;
                        case optimizer::Operations::Cmp3w:
                            swapAndUpdate(n, optimizer::Constant::int32(cmpVal));
                            break;

                        default:
                            MONGO_UNREACHABLE;
                    }
                } else {
                    if (lhsTag == sbe::value::TypeTags::MinKey) {
                        switch (op.op()) {
                            case optimizer::Operations::Lte:
                                swapAndUpdate(n, optimizer::Constant::boolean(true));
                                break;
                            case optimizer::Operations::Gt:
                                swapAndUpdate(n, optimizer::Constant::boolean(false));
                                break;

                            default:
                                break;
                        }
                    } else if (lhsTag == sbe::value::TypeTags::MaxKey) {
                        switch (op.op()) {
                            case optimizer::Operations::Lt:
                                swapAndUpdate(n, optimizer::Constant::boolean(false));
                                break;
                            case optimizer::Operations::Gte:
                                swapAndUpdate(n, optimizer::Constant::boolean(true));
                                break;

                            default:
                                break;
                        }
                    }
                }
            } else if (rhsConst) {
                const auto [rhsTag, rhsVal] = rhsConst->get();

                if (rhsTag == sbe::value::TypeTags::MinKey) {
                    switch (op.op()) {
                        case optimizer::Operations::Lt:
                            swapAndUpdate(n, optimizer::Constant::boolean(false));
                            break;

                        case optimizer::Operations::Gte:
                            swapAndUpdate(n, optimizer::Constant::boolean(true));
                            break;

                        default:
                            break;
                    }
                } else if (rhsTag == sbe::value::TypeTags::MaxKey) {
                    switch (op.op()) {
                        case optimizer::Operations::Lte:
                            swapAndUpdate(n, optimizer::Constant::boolean(true));
                            break;

                        case optimizer::Operations::Gt:
                            swapAndUpdate(n, optimizer::Constant::boolean(false));
                            break;

                        default:
                            break;
                    }
                }
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
    // We can simplify exists(constant) to true if the said constant is not Nothing.
    if (op.name() == "exists" && args.size() == 1 && args[0].is<optimizer::Constant>()) {
        auto [tag, val] = args[0].cast<optimizer::Constant>()->get();
        if (tag != sbe::value::TypeTags::Nothing) {
            swapAndUpdate(n, optimizer::Constant::boolean(true));
        }
    }

    if (op.name() == "newArray") {
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
    }
}

void ExpressionConstEval::prepare(optimizer::ABT&, const optimizer::LambdaAbstraction&) {
    ++_inCostlyCtx;
}

void ExpressionConstEval::transport(optimizer::ABT&,
                                    const optimizer::LambdaAbstraction&,
                                    optimizer::ABT&) {
    --_inCostlyCtx;
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
