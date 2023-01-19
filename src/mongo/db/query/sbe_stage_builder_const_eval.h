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

#pragma once

#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"

namespace mongo {
class CollatorInterface;
}

namespace mongo::stage_builder {

/**
 * Constant folding rewrite that supports custom collation for ABTs that are build by
 * sbe_stage_builder_expression.cpp.
 * Based on optimizer::ConstEval, but without EvaluationNode, FilterNode, PathTraverse, PathCompose*
 * because this nodes are not used in sbe_stage_builder_expression.
 *
 * TODO: Remove and replace with optimizer::ConstEval when it will support collation
 */
class ExpressionConstEval {
public:
    ExpressionConstEval(optimizer::VariableEnvironment& env, const CollatorInterface* collator)
        : _env(env), _collator(collator) {}

    template <typename T, typename... Ts>
    void transport(optimizer::ABT&, const T&, Ts&&...) {}

    void transport(optimizer::ABT& n, const optimizer::Variable& var);

    void prepare(optimizer::ABT&, const optimizer::Let& let);
    void transport(optimizer::ABT& n,
                   const optimizer::Let& let,
                   optimizer::ABT&,
                   optimizer::ABT& in);
    void transport(optimizer::ABT& n,
                   const optimizer::LambdaApplication& app,
                   optimizer::ABT& lam,
                   optimizer::ABT& arg);
    void prepare(optimizer::ABT&, const optimizer::LambdaAbstraction&);
    void transport(optimizer::ABT&, const optimizer::LambdaAbstraction&, optimizer::ABT&);

    void transport(optimizer::ABT& n,
                   const optimizer::BinaryOp& op,
                   optimizer::ABT& lhs,
                   optimizer::ABT& rhs);
    void transport(optimizer::ABT& n,
                   const optimizer::FunctionCall& op,
                   std::vector<optimizer::ABT>& args);
    void transport(optimizer::ABT& n,
                   const optimizer::If& op,
                   optimizer::ABT& cond,
                   optimizer::ABT& thenBranch,
                   optimizer::ABT& elseBranch);

    void prepare(optimizer::ABT&, const optimizer::References& refs);
    void transport(optimizer::ABT& n,
                   const optimizer::References& op,
                   std::vector<optimizer::ABT>&);

    // The tree is passed in as NON-const reference as we will be updating it.
    bool optimize(optimizer::ABT& n);

private:
    struct RefHash {
        size_t operator()(const optimizer::ABT::reference_type& nodeRef) const {
            return nodeRef.hash();
        }
    };

    void swapAndUpdate(optimizer::ABT& n, optimizer::ABT newN);

    optimizer::VariableEnvironment& _env;
    const CollatorInterface* _collator;

    optimizer::opt::unordered_set<const optimizer::Variable*> _singleRef;
    optimizer::opt::unordered_map<const optimizer::Let*, std::vector<const optimizer::Variable*>>
        _letRefs;
    optimizer::opt::
        unordered_map<optimizer::ABT::reference_type, optimizer::ABT::reference_type, RefHash>
            _staleDefs;
    // We collect old ABTs in order to avoid the ABA problem.
    std::vector<optimizer::ABT> _staleABTs;

    bool _inRefBlock{false};
    size_t _inCostlyCtx{0};
    bool _changed{false};
};

}  // namespace mongo::stage_builder
