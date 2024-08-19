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

#include <absl/meta/type_traits.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>

#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/utils/abt_compare.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/util/assert_util.h"

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

void ConstEval::constFold(ABT& n) {
    VariableEnvironment env = VariableEnvironment::build(n, false /*computeLastRefs*/);
    ConstEval instance(env);
    instance.optimize(n);
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
            if (!_canInlineEval || _canInlineEval(*k)) {
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

ConstEval::Nullability ConstEval::transport(ABT& n, const Constant& c) {
    // Non-nothing constants are non-nullable.
    if (c.isNothing()) {
        return Nullability::kNullable;
    }
    return Nullability::kNonNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n, const Variable& var) {
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
            swapAndUpdate(n, def.definition.copy());
            if (!constant->isNothing()) {
                return Nullability::kNonNullable;
            }
        } else if (auto variable = def.definition.cast<Variable>(); variable && !_inRefBlock) {
            // This is a indirection to another variable. So we can skip, but first remember that we
            // inlined this variable so that we won't try to replace it with a common expression and
            // revert the inlining.
            if (_renamedProj) {
                _renamedProj(var.name(), variable->name());
            }
            _inlinedDefs.emplace(def.definition);
            swapAndUpdate(n, def.definition.copy());
        } else if (_singleRef.erase(&var)) {
            // If this is the only reference to some expression then substitute the variable, but
            // first remember that we inlined this expression so that we won't try to replace it
            // with a common expression and revert the inlining.
            _inlinedDefs.emplace(def.definition);
            swapAndUpdate(n, def.definition.copy());
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
    // Variables that don't have a non-nothing constant definition are nullable.
    return Nullability::kNullable;
}

void ConstEval::prepare(ABT&, const Let& let) {
    _letRefs[&let] = {};
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            Let& let,
                                            ConstEval::Nullability,
                                            ConstEval::Nullability inNullability) {
    auto& in = let.in();

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
    // Let's nullability is a function only of the nullability of the in expression.
    // Ex: n == let x='a' (bind expr) in x+2 (in expr)
    // While the bind expr is non-nullable, the in expr determines the final nullability because it
    // may evaluate to Nothing with mixed types.
    return inNullability;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            LambdaApplication& app,
                                            ConstEval::Nullability,
                                            ConstEval::Nullability) {
    auto& lam = app.getLambda();
    auto& arg = app.getArgument();

    // If the 'lam' expression is LambdaAbstraction then we can do the inplace beta reduction.
    // TODO - missing alpha conversion so for now assume globally unique names.
    if (auto lambda = lam.cast<LambdaAbstraction>(); lambda) {
        auto result = make<Let>(lambda->varName(),
                                std::exchange(arg, make<Blackhole>()),
                                std::exchange(lambda->getBody(), make<Blackhole>()));

        swapAndUpdate(n, std::move(result));
    }
    // Applying the lambda abstraction over an arg could return a Nullable output regardless of the
    // arg's nullability.
    return Nullability::kNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n, UnaryOp& op, ConstEval::Nullability) {
    auto& child = op.getChild();

    switch (op.op()) {
        case Operations::Not: {
            if (const auto childConst = child.cast<Constant>();
                childConst && childConst->isValueBool()) {
                swapAndUpdate(n, Constant::boolean(!childConst->getValueBool()));
                return Nullability::kNonNullable;
            }
            break;
        }

            // Could also constant fold arithmetic negation.

        default:
            break;
    }
    // If the child is not a boolean constant, return nullable.
    // Not will return Nothing on a non-logical input. Neg will return Nothing if the child op is
    // not a comparison or can't be negated (i.e. EqMember)
    return Nullability::kNullable;
}

// Specific transport for binary operation
// The const correctness is probably wrong (as const ABT& lhs, const ABT& rhs does not work for
// some reason but we can fix it later).
ConstEval::Nullability ConstEval::transport(ABT& n,
                                            BinaryOp& op,
                                            ConstEval::Nullability lhsNullability,
                                            ConstEval::Nullability rhsNullability) {
    auto& lhs = op.getLeftChild();
    auto& rhs = op.getRightChild();

    // Determine the tightest claim we can make about Nullability given lhs and rhs nullability.
    // For comparison and logical ops:
    // - If either lhs or rhs is nullable, the binary op output is nullable.
    // - If both lhs and rhs are non-nullable, the binary op output is non-nullable.
    // Arithmetic ops may return Nothing even if both inputs are non-nullable.
    // - For example, exprs with mixed types like 5 + "a" would return Nothing even though the
    // arguments are non-nullable.
    auto defaultNullability = [&]() {
        if (lhsNullability == Nullability::kNullable || rhsNullability == Nullability::kNullable) {
            return Nullability::kNullable;
        }
        return Nullability::kNonNullable;
    };

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
                if (resultType != sbe::value::TypeTags::Nothing) {
                    return Nullability::kNonNullable;
                }
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
                if (resultType != sbe::value::TypeTags::Nothing) {
                    return Nullability::kNonNullable;
                }
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
                if (resultType != sbe::value::TypeTags::Nothing) {
                    return Nullability::kNonNullable;
                }
            }
            break;
        }

        case Operations::Or: {
            // Nothing and short-circuiting semantics of the 'or' operation in SBE allow us to
            // interrogate 'lhs' only provided the right side is not a constant.
            if (const auto* lhsConst = lhs.cast<Constant>()) {
                if (const auto [lhsTag, lhsValue] = lhsConst->get();
                    lhsTag == sbe::value::TypeTags::Boolean) {
                    const bool lhsBool = sbe::value::bitcastTo<bool>(lhsValue);
                    if (const auto* rhsConst = rhs.cast<Constant>()) {
                        if (const auto [rhsTag, rhsValue] = rhsConst->get();
                            rhsTag == sbe::value::TypeTags::Boolean) {
                            // Both left and right sides are boolean constants.
                            swapAndUpdate(n,
                                          Constant::boolean(lhsBool ||
                                                            sbe::value::bitcastTo<bool>(rhsValue)));
                            return Nullability::kNonNullable;
                        }
                    } else {
                        // Right side is not constant.
                        if (lhsBool) {
                            // true || rhs -> true.
                            swapAndUpdate(n, Constant::boolean(true));
                            return Nullability::kNonNullable;
                        } else {
                            // false || rhs -> rhs.
                            swapAndUpdate(n, std::exchange(rhs, make<Blackhole>()));
                            return rhsNullability;
                        }
                    }
                }
            }

            // Given SBE short-circuiting semantics, we can interrogate the lhs and perform
            // optimizations if we know it is non-nullable.
            // For the disjunctive case, we short-circuit the lhs on Nothing and on True, and fall
            // through for all other values (False, 5, "a").
            // - In 5 || true, we fall through and return the final true.
            // - In 5 || false, we fall through and return the final false.
            if (lhsNullability == Nullability::kNonNullable) {
                if (const auto* rhsConst = rhs.cast<Constant>()) {
                    if (const auto [rhsTag, rhsValue] = rhsConst->get();
                        rhsTag == sbe::value::TypeTags::Boolean) {
                        const bool rhsBool = sbe::value::bitcastTo<bool>(rhsValue);
                        if (rhsBool) {
                            // non-nothing lhs || true -> true.
                            swapAndUpdate(n, Constant::boolean(true));
                        } else if (!lhs.is<Constant>()) {
                            // non-const, non-nothing lhs || false -> lhs.
                            swapAndUpdate(n, std::exchange(lhs, make<Blackhole>()));
                        }
                        return Nullability::kNonNullable;
                    }
                }
            } else if (const auto* rhsConst = rhs.cast<Constant>()) {
                // Left side is not constant and right side is a "false" constant.
                if (const auto [rhsTag, rhsValue] = rhsConst->get();
                    rhsTag == sbe::value::TypeTags::Boolean &&
                    !sbe::value::bitcastTo<bool>(rhsValue) && !lhs.is<Constant>()) {
                    // x || false -> x.
                    swapAndUpdate(n, std::exchange(lhs, make<Blackhole>()));
                    return lhsNullability;
                }
            }
            return defaultNullability();
        }

        case Operations::And: {
            // Nothing and short-circuiting semantics of the 'and' operation in SBE allow us to
            // interrogate 'lhs' only provided the right side is not a constant.
            if (const auto* lhsConst = lhs.cast<Constant>()) {
                if (const auto [lhsTag, lhsValue] = lhsConst->get();
                    lhsTag == sbe::value::TypeTags::Boolean) {
                    const bool lhsBool = sbe::value::bitcastTo<bool>(lhsValue);
                    if (const auto* rhsConst = rhs.cast<Constant>()) {
                        if (const auto [rhsTag, rhsValue] = rhsConst->get();
                            rhsTag == sbe::value::TypeTags::Boolean) {
                            // Both left and right sides are boolean constants.
                            swapAndUpdate(n,
                                          Constant::boolean(lhsBool &&
                                                            sbe::value::bitcastTo<bool>(rhsValue)));
                            return Nullability::kNonNullable;
                        }
                    } else {
                        // Right side is not constant.
                        if (lhsBool) {
                            // true && rhs -> rhs.
                            swapAndUpdate(n, std::exchange(rhs, make<Blackhole>()));
                            return rhsNullability;
                        } else {
                            // false && rhs -> false.
                            swapAndUpdate(n, Constant::boolean(false));
                            return Nullability::kNonNullable;
                        }
                    }
                }
            }

            // Given SBE short-circuiting semantics, we can interrogate the lhs and perform
            // optimizations if we know it is non-nullable.
            // For the conjunctive case, we short-circuit the lhs on Nothing and on False, and fall
            // through for all other values (True, 5, "a").
            // - In 5 && false, we fall through and return the final false.
            // - In 5 && true, we fall through and return the final true.
            if (lhsNullability == Nullability::kNonNullable) {
                if (const auto* rhsConst = rhs.cast<Constant>()) {
                    if (const auto [rhsTag, rhsValue] = rhsConst->get();
                        rhsTag == sbe::value::TypeTags::Boolean) {
                        const bool rhsBool = sbe::value::bitcastTo<bool>(rhsValue);
                        if (rhsBool) {
                            if (!lhs.is<Constant>()) {
                                // non-const, non-nothing lhs && true -> lhs.
                                swapAndUpdate(n, std::exchange(lhs, make<Blackhole>()));
                            }
                        } else {
                            // non-nothing lhs && false -> false.
                            swapAndUpdate(n, Constant::boolean(false));
                        }
                        return Nullability::kNonNullable;
                    }
                }
            } else if (const auto* rhsConst = rhs.cast<Constant>()) {
                // Left side is not constant and right side is a "true" constant.
                if (const auto [rhsTag, rhsValue] = rhsConst->get();
                    rhsTag == sbe::value::TypeTags::Boolean &&
                    sbe::value::bitcastTo<bool>(rhsValue) && !lhs.is<Constant>()) {
                    // x && true -> x.
                    swapAndUpdate(n, std::exchange(lhs, make<Blackhole>()));
                    return lhsNullability;
                }
            }
            return defaultNullability();
        }

        case Operations::Eq: {
            auto cmpVal = cmpEqFast(lhs, rhs);
            if (cmpVal != CmpResult::kIncomparable) {
                swapAndUpdate(n, Constant::boolean(cmpVal == CmpResult::kTrue));
                return Nullability::kNonNullable;
            }
            return defaultNullability();
        }

        case Operations::Lt:
        case Operations::Lte:
        case Operations::Gt:
        case Operations::Gte:
        case Operations::Cmp3w: {
            auto cmpVal = cmp3wFast(op.op(), lhs, rhs);
            if (cmpVal != CmpResult::kIncomparable) {
                if (op.op() == Operations::Cmp3w) {
                    swapAndUpdate(n, Constant::int32(static_cast<int32_t>(cmpVal)));
                } else {
                    swapAndUpdate(n, Constant::boolean(cmpVal == CmpResult::kTrue));
                }
                return Nullability::kNonNullable;
            }
            return defaultNullability();
        }

        case Operations::FillEmpty:
            if (const auto* lhsConst = lhs.cast<Constant>()) {
                if (auto [tag, val] = lhsConst->get(); tag != sbe::value::TypeTags::Nothing) {
                    swapAndUpdate(n, std::exchange(lhs, make<Blackhole>()));
                    return Nullability::kNonNullable;
                }
            }
            break;

        default:
            // Not implemented.
            break;
    }
    return Nullability::kNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            FunctionCall& op,
                                            std::vector<ConstEval::Nullability> argsNullability) {
    auto& args = op.nodes();

    if (op.name() == "exists") {
        if (args.size() == 1 && args[0].is<Constant>()) {
            // We can simplify exists(constant).
            const bool v = args[0].cast<Constant>()->get().first != sbe::value::TypeTags::Nothing;
            swapAndUpdate(n, Constant::boolean(v));
            return Nullability::kNonNullable;
        }
    } else if (op.name() == "newArray") {
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
            return Nullability::kNonNullable;
        }
    } else if (op.name() == "traverseP") {
        // TraverseP with an identity lambda. Replace with the input.
        if (const auto* lambdaPtr = args.at(1).cast<LambdaAbstraction>()) {
            if (const auto* varPtr = lambdaPtr->getBody().cast<Variable>();
                varPtr != nullptr && varPtr->name() == lambdaPtr->varName()) {
                swapAndUpdate(n, args.front());
                // Return the nullability value of the input.
                return argsNullability.front();
            }
        }
    } else if (op.name() == "isArray") {
        if (args.size() == 1 && args[0].is<Constant>()) {
            const auto tag = args[0].cast<Constant>()->get().first;
            const bool v = tag == sbe::value::TypeTags::Array ||
                tag == sbe::value::TypeTags::ArraySet || tag == sbe::value::TypeTags::ArrayMultiSet;
            swapAndUpdate(n, Constant::boolean(v));
            return Nullability::kNonNullable;
        }
    }
    // The isInList check currently only pertains to parameterized InMatchExpressions, whose
    // equalities list is bound into the SBE runtime environment as InList. In this case, no
    // constant folding will be performed for op.name() == "isInList" because InList is never
    // used in the optimizer; it is generated in a later phase in SBE.
    //
    // If the child node is a Constant that's not of type inList, the check can be ignored and
    // set directly to false.
    else if (op.name() == "isInList") {
        if (args.size() == 1 && args[0].is<Constant>()) {
            const auto tag = args[0].cast<Constant>()->get().first;
            swapAndUpdate(n, Constant::boolean(tag == sbe::value::TypeTags::inList));
            return Nullability::kNonNullable;
        }
    } else if (op.name() == kParameterFunctionName) {
        return Nullability::kNonNullable;
    }
    return Nullability::kNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            If& op,
                                            ConstEval::Nullability condNullability,
                                            ConstEval::Nullability thenNullability,
                                            ConstEval::Nullability elseNullability) {
    auto& cond = op.getCondChild();
    auto& thenBranch = op.getThenChild();
    auto& elseBranch = op.getElseChild();

    if (const auto* condConst = cond.cast<Constant>()) {
        // If the condition is a boolean constant we can simplify.
        if (const auto [condTag, condValue] = condConst->get();
            condTag == sbe::value::TypeTags::Boolean) {
            if (sbe::value::bitcastTo<bool>(condValue)) {
                // If true then x else y -> x.
                swapAndUpdate(n, std::exchange(thenBranch, make<Blackhole>()));
                return thenNullability;
            } else {
                // If false then x else y -> y.
                swapAndUpdate(n, std::exchange(elseBranch, make<Blackhole>()));
                return elseNullability;
            }
        }
    } else if (thenBranch.is<Constant>() && elseBranch.is<Constant>()) {
        // If both branches are boolean constants then we can simplify.
        if (const auto [tag1, val1] = thenBranch.cast<Constant>()->get();
            tag1 == sbe::value::TypeTags::Boolean) {
            const bool v1 = sbe::value::bitcastTo<bool>(val1);
            if (const auto [tag2, val2] = elseBranch.cast<Constant>()->get();
                tag2 == sbe::value::TypeTags::Boolean) {
                const bool v2 = sbe::value::bitcastTo<bool>(val2);
                if (v1) {
                    if (v2) {
                        // if (x) then true else true -> true.
                        swapAndUpdate(n, Constant::boolean(true));
                        return Nullability::kNonNullable;
                    } else {
                        // if (x) then true else false -> (x).
                        swapAndUpdate(n, std::move(cond));
                        return condNullability;
                    }
                } else if (v2) {
                    // If (x) then false else true -> !(x).
                    swapAndUpdate(n, make<UnaryOp>(Operations::Not, std::move(cond)));
                    return condNullability;
                } else {
                    // if (x) then false else false -> false.
                    swapAndUpdate(n, Constant::boolean(false));
                    return Nullability::kNonNullable;
                }
            }
        }
    } else if (const auto* condNot = cond.cast<UnaryOp>();
               condNot != nullptr && condNot->op() == Operations::Not) {
        // If (Not (x)) then y else z -> if (x) then z else y.
        swapAndUpdate(
            n,
            make<If>(std::move(condNot->getChild()), std::move(elseBranch), std::move(thenBranch)));
    }
    return Nullability::kNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            EvalPath& op,
                                            ConstEval::Nullability pathNullability,
                                            ConstEval::Nullability) {
    auto& path = op.getPath();

    if (const auto* pathConstPtr = path.cast<PathConstant>()) {
        // PathConst does not depend on its parent, so replace with the PathConst's child.
        swapAndUpdate(n, pathConstPtr->getConstant());
        return pathNullability;
    }
    return Nullability::kNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            EvalFilter& op,
                                            ConstEval::Nullability pathNullability,
                                            ConstEval::Nullability) {
    auto& path = op.getPath();

    if (const auto* pathConstPtr = path.cast<PathConstant>()) {
        // PathConst does not depend on its parent, so replace with the PathConst's child.
        swapAndUpdate(n, pathConstPtr->getConstant());
        return pathNullability;
    }
    return Nullability::kNullable;
}

void ConstEval::prepare(ABT&, const PathTraverse&) {
    ++_inCostlyCtx;
}

ConstEval::Nullability ConstEval::transport(ABT&,
                                            const PathTraverse&,
                                            ConstEval::Nullability nullability) {
    --_inCostlyCtx;
    return nullability;
}

template <bool v>
static ConstEval::Nullability constEvalComposition(ABT& n,
                                                   ABT& lhs,
                                                   ABT& rhs,
                                                   ConstEval::Nullability lhsNullability,
                                                   ConstEval::Nullability rhsNullability) {
    ABT c = make<PathConstant>(Constant::boolean(v));
    if (lhs == c || rhs == c) {
        std::swap(n, c);
        return ConstEval::Nullability::kNonNullable;
    }

    c = make<PathConstant>(Constant::boolean(!v));
    if (lhs == c) {
        n = std::move(rhs);
        return rhsNullability;
    } else if (rhs == c) {
        n = std::move(lhs);
        return lhsNullability;
    }
    return ConstEval::Nullability::kNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            PathComposeM& op,
                                            ConstEval::Nullability lhsNullability,
                                            ConstEval::Nullability rhsNullability) {
    auto& lhs = op.getPath1();
    auto& rhs = op.getPath2();

    return constEvalComposition<false>(n, lhs, rhs, lhsNullability, rhsNullability);
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            PathComposeA& op,
                                            ConstEval::Nullability lhsNullability,
                                            ConstEval::Nullability rhsNullability) {
    auto& lhs = op.getPath1();
    auto& rhs = op.getPath2();

    return constEvalComposition<true>(n, lhs, rhs, lhsNullability, rhsNullability);
}

void ConstEval::prepare(ABT&, const LambdaAbstraction&) {
    ++_inCostlyCtx;
}

ConstEval::Nullability ConstEval::transport(ABT&,
                                            const LambdaAbstraction&,
                                            ConstEval::Nullability) {
    --_inCostlyCtx;
    // The lambda abstraction expression is always non-nothing.
    return Nullability::kNonNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            FilterNode& op,
                                            ConstEval::Nullability,
                                            ConstEval::Nullability) {
    auto& child = op.getChild();
    auto& expr = op.getFilter();

    if (expr == Constant::boolean(true)) {
        // Remove trivially true filter.

        // First, pull out the child and put in a blackhole.
        auto result = std::exchange(child, make<Blackhole>());

        // Replace the filter node itself with the extracted child.
        swapAndUpdate(n, std::move(result));
    }
    return Nullability::kNullable;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            EvaluationNode& op,
                                            ConstEval::Nullability,
                                            ConstEval::Nullability) {
    auto& child = op.getChild();

    if (_noRefProj.erase(&op)) {
        // The evaluation node is unused so replace it with its own child.
        if (_erasedProj) {
            _erasedProj(op.getProjectionName());
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
    return Nullability::kNullable;
}

void ConstEval::prepare(ABT&, const References& refs) {
    // It is structurally impossible to nest References nodes.
    invariant(!_inRefBlock);
    _inRefBlock = true;
}

ConstEval::Nullability ConstEval::transport(ABT& n,
                                            const References& op,
                                            std::vector<ConstEval::Nullability>) {
    invariant(_inRefBlock);
    _inRefBlock = false;
    return Nullability::kNullable;
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
