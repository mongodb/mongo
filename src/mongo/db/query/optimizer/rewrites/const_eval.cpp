/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {
bool ConstEval::optimize(ABT& n) {
    invariant(_letRefs.empty());
    invariant(_projectRefs.empty());
    invariant(_singleRef.empty());
    invariant(_noRefProj.empty());
    invariant(!_inRefBlock);
    invariant(_inCostlyCtx == 0);
    invariant(_staleDefs.empty());
    invariant(_staleABTs.empty());
    invariant(_seenProjects.empty());
    invariant(_inlinedDefs.empty());

    _changed = false;

    // We run the transport<true> that will pass the reference to ABT to specific transport
    // functions. The reference serves as a conceptual 'this' pointer allowing the transport
    // function to change the node itself.
    algebra::transport<true>(n, *this);

    // Test if there are any projections with no references. If so remove them from the tree
    removeUnusedEvalNodes();

    invariant(_letRefs.empty());
    invariant(_projectRefs.empty());

    while (_changed) {
        _env.rebuild(n);

        if (_singleRef.empty() && _noRefProj.empty()) {
            break;
        }
        _changed = false;
        algebra::transport<true>(n, *this);
        removeUnusedEvalNodes();
    }

    // TODO: should we be clearing here?
    _singleRef.clear();

    _staleDefs.clear();
    _staleABTs.clear();
    return _changed;
}

void ConstEval::removeUnusedEvalNodes() {
    for (auto&& [k, v] : _projectRefs) {
        if (v.size() == 0) {
            // Schedule node replacement as it has not references.
            _noRefProj.emplace(k);
            _changed = true;
        } else if (v.size() == 1) {
            // Do not inline nodes which can become Sargable.
            // TODO: consider caching.
            // TODO: consider deriving IndexingAvailability.
            if (!_disableInline || !_disableInline(k->getProjection())) {
                // Schedule node inlining as there is exactly one reference.
                _singleRef.emplace(v.front());
                _changed = true;
            }
        }
    }

    _projectRefs.clear();
    _seenProjects.clear();
    _inlinedDefs.clear();
}

void ConstEval::transport(ABT& n, const Variable& var) {
    auto def = _env.getDefinition(var);

    if (!def.definition.empty()) {
        // See if we have already manipulated this definition and if so then use the newer version.
        if (auto it = _staleDefs.find(def.definition); it != _staleDefs.end()) {
            def.definition = it->second;
        }
        if (auto it = _staleDefs.find(def.definedBy); it != _staleDefs.end()) {
            def.definedBy = it->second;
        }

        if (auto constant = def.definition.cast<Constant>(); constant && !_inRefBlock) {
            // If we find the definition and it is a simple constant then substitute the variable.
            swapAndUpdate(n, def.definition);
        } else if (auto variable = def.definition.cast<Variable>(); variable && !_inRefBlock) {
            // This is a indirection to another variable. So we can skip, but first remember that we
            // inlined this variable so that we won't try to replace it with a common expression and
            // revert the inlining.
            _inlinedDefs.emplace(def.definition);
            swapAndUpdate(n, def.definition);
        } else if (_singleRef.erase(&var)) {
            // If this is the only reference to some expression then substitute the variable, but
            // first remember that we inlined this expression so that we won't try to replace it
            // with a common expression and revert the inlining.
            _inlinedDefs.emplace(def.definition);
            swapAndUpdate(n, def.definition);
        } else if (auto let = def.definedBy.cast<Let>(); let) {
            invariant(_letRefs.count(let));
            _letRefs[let].emplace_back(&var);
        } else if (auto project = def.definedBy.cast<EvaluationNode>(); project) {
            invariant(_projectRefs.count(project));
            _projectRefs[project].emplace_back(&var);

            // If we are in the ref block we do not want to inline even if there is only a single
            // reference. Similarly, we do not want to inline any variable under traverse.
            if (_inRefBlock || _inCostlyCtx > 0) {
                _projectRefs[project].emplace_back(&var);
            }
        }
    }
}

void ConstEval::prepare(ABT&, const Let& let) {
    _letRefs[&let] = {};
}

void ConstEval::transport(ABT& n, const Let& let, ABT& bind, ABT& in) {
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

        // We don't want to make a copy of 'in' as it may be arbitrarily large. Also, we cannot
        // move it out as it is part of the Let object and we do not want to invalidate any
        // assumptions the Let may have about its structure. Hence we swap it for the "special"
        // Blackhole object. The Blackhole does nothing, it just plugs the hole left in the 'in'
        // place.
        auto result = std::exchange(in, make<Blackhole>());

        // Swap the current node (n) for the result.
        swapAndUpdate(n, std::move(result));
    } else if (letRefs.size() == 1) {
        // The bind expression has been referenced exactly once so schedule it for inlining.
        _singleRef.emplace(letRefs.front());
        _changed = true;
    }
    _letRefs.erase(&let);
}

void ConstEval::transport(ABT& n, const LambdaApplication& app, ABT& lam, ABT& arg) {
    // If the 'lam' expression is LambdaAbstraction then we can do the inplace beta reduction.
    // TODO - missing alpha conversion so for now assume globally unique names.
    if (auto lambda = lam.cast<LambdaAbstraction>(); lambda) {
        auto result = make<Let>(lambda->varName(),
                                std::exchange(arg, make<Blackhole>()),
                                std::exchange(lambda->getBody(), make<Blackhole>()));

        swapAndUpdate(n, std::move(result));
    }
}

// Specific transport for binary operation
// The const correctness is probably wrong (as const ABT& lhs, const ABT& rhs does not work for
// some reason but we can fix it later).
void ConstEval::transport(ABT& n, const BinaryOp& op, ABT& lhs, ABT& rhs) {

    switch (op.op()) {
        case Operations::Add: {
            // Let say we want to recognize ConstLhs + ConstRhs and replace it with the result of
            // addition.
            auto lhsConst = lhs.cast<Constant>();
            auto rhsConst = rhs.cast<Constant>();
            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericAdd(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, make<Constant>(resultType, resultValue));
            }
            break;
        }

        case Operations::Sub: {
            // Let say we want to recognize ConstLhs - ConstRhs and replace it with the result of
            // subtraction.
            auto lhsConst = lhs.cast<Constant>();
            auto rhsConst = rhs.cast<Constant>();

            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericSub(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, make<Constant>(resultType, resultValue));
            }
            break;
        }

        case Operations::Mult: {
            // Let say we want to recognize ConstLhs * ConstRhs and replace it with the result of
            // multiplication.
            auto lhsConst = lhs.cast<Constant>();
            auto rhsConst = rhs.cast<Constant>();

            if (lhsConst && rhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                auto [rhsTag, rhsValue] = rhsConst->get();
                auto [_, resultType, resultValue] =
                    sbe::value::genericMul(lhsTag, lhsValue, rhsTag, rhsValue);
                swapAndUpdate(n, make<Constant>(resultType, resultValue));
            }
            break;
        }

        case Operations::Or: {
            // Nothing and short-circuiting semantics of the 'or' operation in SBE allow us to
            // interrogate 'lhs' only.
            if (auto lhsConst = lhs.cast<Constant>(); lhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                if (lhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(lhsValue)) {
                    // false || rhs -> rhs
                    swapAndUpdate(n, std::exchange(rhs, make<Blackhole>()));
                } else if (lhsTag == sbe::value::TypeTags::Boolean &&
                           sbe::value::bitcastTo<bool>(lhsValue)) {
                    // true || rhs -> true
                    swapAndUpdate(n, Constant::boolean(true));
                }
            }
            break;
        }

        case Operations::And: {
            // Nothing and short-circuiting semantics of the 'and' operation in SBE allow us to
            // interrogate 'lhs' only.
            if (auto lhsConst = lhs.cast<Constant>(); lhsConst) {
                auto [lhsTag, lhsValue] = lhsConst->get();
                if (lhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(lhsValue)) {
                    // false && rhs -> false
                    swapAndUpdate(n, Constant::boolean(false));
                } else if (lhsTag == sbe::value::TypeTags::Boolean &&
                           sbe::value::bitcastTo<bool>(lhsValue)) {
                    // true && rhs -> rhs
                    swapAndUpdate(n, std::exchange(rhs, make<Blackhole>()));
                }
            }
            break;
        }

        case Operations::Eq: {
            if (lhs == rhs) {
                // If the subtrees are equal, we can conclude that their result is equal because we
                // have only pure functions.
                swapAndUpdate(n, Constant::boolean(true));
            } else if (lhs.is<Constant>() && rhs.is<Constant>()) {
                // We have two constants which are not equal.
                swapAndUpdate(n, Constant::boolean(false));
            }
            break;
        }

        case Operations::Lt:
        case Operations::Lte:
        case Operations::Gt:
        case Operations::Gte:
        case Operations::Cmp3w: {
            const auto lhsConst = lhs.cast<Constant>();
            const auto rhsConst = rhs.cast<Constant>();

            if (lhsConst) {
                const auto [lhsTag, lhsVal] = lhsConst->get();

                if (rhsConst) {
                    const auto [rhsTag, rhsVal] = rhsConst->get();

                    const auto [compareTag, compareVal] =
                        sbe::value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
                    const auto cmpVal = sbe::value::bitcastTo<int32_t>(compareVal);

                    switch (op.op()) {
                        case Operations::Lt:
                            swapAndUpdate(n, Constant::boolean(cmpVal < 0));
                            break;
                        case Operations::Lte:
                            swapAndUpdate(n, Constant::boolean(cmpVal <= 0));
                            break;
                        case Operations::Gt:
                            swapAndUpdate(n, Constant::boolean(cmpVal > 0));
                            break;
                        case Operations::Gte:
                            swapAndUpdate(n, Constant::boolean(cmpVal >= 0));
                            break;
                        case Operations::Cmp3w:
                            swapAndUpdate(n, Constant::int32(cmpVal));
                            break;

                        default:
                            MONGO_UNREACHABLE;
                    }
                } else {
                    if (lhsTag == sbe::value::TypeTags::MinKey) {
                        switch (op.op()) {
                            case Operations::Lte:
                                swapAndUpdate(n, Constant::boolean(true));
                                break;
                            case Operations::Gt:
                                swapAndUpdate(n, Constant::boolean(false));
                                break;

                            default:
                                break;
                        }
                    } else if (lhsTag == sbe::value::TypeTags::MaxKey) {
                        switch (op.op()) {
                            case Operations::Lt:
                                swapAndUpdate(n, Constant::boolean(false));
                                break;
                            case Operations::Gte:
                                swapAndUpdate(n, Constant::boolean(true));
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
                        case Operations::Lt:
                            swapAndUpdate(n, Constant::boolean(false));
                            break;

                        case Operations::Gte:
                            swapAndUpdate(n, Constant::boolean(true));
                            break;

                        default:
                            break;
                    }
                } else if (rhsTag == sbe::value::TypeTags::MaxKey) {
                    switch (op.op()) {
                        case Operations::Lte:
                            swapAndUpdate(n, Constant::boolean(true));
                            break;

                        case Operations::Gt:
                            swapAndUpdate(n, Constant::boolean(false));
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

void ConstEval::transport(ABT& n, const FunctionCall& op, std::vector<ABT>& args) {
    // We can simplify exists(constant) to true if the said constant is not Nothing.
    if (op.name() == "exists" && args.size() == 1 && args[0].is<Constant>()) {
        auto [tag, val] = args[0].cast<Constant>()->get();
        if (tag != sbe::value::TypeTags::Nothing) {
            swapAndUpdate(n, Constant::boolean(true));
        }
    }

    if (op.name() == "newArray") {
        bool allConstants = true;
        for (const ABT& arg : op.nodes()) {
            if (!arg.is<Constant>()) {
                allConstants = false;
                break;
            }
        }

        if (allConstants) {
            // All arguments are constants. Replace with an array constant.

            sbe::value::Array array;
            for (const ABT& arg : op.nodes()) {
                auto [tag, val] = arg.cast<Constant>()->get();
                // Copy the value before inserting into the array.
                auto [tagCopy, valCopy] = sbe::value::copyValue(tag, val);
                array.push_back(tagCopy, valCopy);
            }

            auto [tag, val] = sbe::value::makeCopyArray(array);
            swapAndUpdate(n, make<Constant>(tag, val));
        }
    }
}

void ConstEval::transport(ABT& n, const If& op, ABT& cond, ABT& thenBranch, ABT& elseBranch) {
    // If the condition is a boolean constant we can simplify.
    if (auto condConst = cond.cast<Constant>(); condConst) {
        auto [condTag, condValue] = condConst->get();
        if (condTag == sbe::value::TypeTags::Boolean && sbe::value::bitcastTo<bool>(condValue)) {
            // if true -> thenBranch
            swapAndUpdate(n, std::exchange(thenBranch, make<Blackhole>()));
        } else if (condTag == sbe::value::TypeTags::Boolean &&
                   !sbe::value::bitcastTo<bool>(condValue)) {
            // if false -> elseBranch
            swapAndUpdate(n, std::exchange(elseBranch, make<Blackhole>()));
        }
    }
}

void ConstEval::prepare(ABT&, const PathTraverse&) {
    ++_inCostlyCtx;
}

void ConstEval::transport(ABT&, const PathTraverse&, ABT&) {
    --_inCostlyCtx;
}

template <bool v>
static void constEvalComposition(ABT& n, ABT& lhs, ABT& rhs) {
    ABT c = make<PathConstant>(Constant::boolean(v));
    if (lhs == c || rhs == c) {
        std::swap(n, c);
        return;
    }

    c = make<PathConstant>(Constant::boolean(!v));
    if (lhs == c) {
        n = std::move(rhs);
    } else if (rhs == c) {
        n = std::move(lhs);
    }
}

void ConstEval::transport(ABT& n, const PathComposeM& op, ABT& lhs, ABT& rhs) {
    constEvalComposition<false>(n, lhs, rhs);
}

void ConstEval::transport(ABT& n, const PathComposeA& op, ABT& lhs, ABT& rhs) {
    constEvalComposition<true>(n, lhs, rhs);
}

void ConstEval::prepare(ABT&, const LambdaAbstraction&) {
    ++_inCostlyCtx;
}

void ConstEval::transport(ABT&, const LambdaAbstraction&, ABT&) {
    --_inCostlyCtx;
}

void ConstEval::transport(ABT& n, const EvaluationNode& op, ABT& child, ABT& expr) {
    if (_noRefProj.erase(&op)) {
        // The evaluation node is unused so replace it with its own child.
        if (_erasedProjNames != nullptr) {
            _erasedProjNames->insert(op.getProjectionName());
        }

        // First, pull out the child and put in a blackhole.
        auto result = std::exchange(child, make<Blackhole>());

        // Replace the evaluation node itself with the extracted child.
        swapAndUpdate(n, std::move(result));
    } else {
        if (!_projectRefs.count(&op)) {
            _projectRefs[&op] = {};
        }

        // Do not consider simple constants or variable references for elimination.
        if (!op.getProjection().is<Constant>() && !op.getProjection().is<Variable>()) {
            // Try to find a projection with the same expression as the current 'op' node and
            // substitute it with a variable pointing to that source projection.
            if (auto source = _seenProjects.find(&op); source != _seenProjects.end() &&
                // Make sure that the matched projection is visible to the current 'op'.
                _env.getProjections(op).count((*source)->getProjectionName()) &&
                // If we already inlined the matched projection, we don't want to use it as a source
                // for common expression as it will negate the inlining.
                !_inlinedDefs.count((*source)->getProjection().ref())) {
                invariant(_projectRefs.count(*source));

                auto var = make<Variable>((*source)->getProjectionName());
                // Source now will have an extra reference from the newly constructed projection.
                _projectRefs[*source].emplace_back(var.cast<Variable>());

                auto newN = make<EvaluationNode>(op.getProjectionName(),
                                                 std::move(var),
                                                 std::exchange(child, make<Blackhole>()));
                // The new projection node should inherit the references from the old node.
                _projectRefs[newN.cast<EvaluationNode>()] = std::move(_projectRefs[&op]);
                _projectRefs.erase(&op);

                swapAndUpdate(n, std::move(newN));
            } else {
                _seenProjects.emplace(&op);
            }
        }
    }
}

void ConstEval::prepare(ABT&, const References& refs) {
    // It is structurally impossible to nest References nodes.
    invariant(!_inRefBlock);
    _inRefBlock = true;
}
void ConstEval::transport(ABT& n, const References& op, std::vector<ABT>&) {
    invariant(_inRefBlock);
    _inRefBlock = false;
}

void ConstEval::swapAndUpdate(ABT& n, ABT newN) {
    // Record the mapping from the old to the new.
    invariant(_staleDefs.count(n.ref()) == 0);
    invariant(_staleDefs.count(newN.ref()) == 0);

    _staleDefs[n.ref()] = newN.ref();

    // Do the swap.
    std::swap(n, newN);

    // newN now contains the old ABT
    _staleABTs.emplace_back(std::move(newN));

    _changed = true;
}
}  // namespace mongo::optimizer
