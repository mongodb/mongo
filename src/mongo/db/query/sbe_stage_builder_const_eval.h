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

#include <absl/container/node_hash_map.h>
#include <cstddef>
#include <vector>

#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"

namespace mongo {
class CollatorInterface;
}

namespace mongo::stage_builder {

/**
 * 'ExpressionConstEval' is a constant-folding rewrite designed to be used by the SBE stage builder.
 * This class was originally adapted from optimizer::ConstEval.
 *
 * Note that ExpressionConstEval assumes 'ComparisonOpSemantics::kTypeBracketing' semantics for
 * comparison ops.
 */
class ExpressionConstEval {
public:
    ExpressionConstEval(const CollatorInterface* collator) : _collator(collator) {}

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

    void transport(optimizer::ABT& n, const optimizer::UnaryOp& op, optimizer::ABT& child);
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

    optimizer::DefinitionsMap _variableDefinitions;
    const CollatorInterface* _collator;

    optimizer::opt::unordered_set<const optimizer::Variable*> _singleRef;
    optimizer::opt::unordered_map<const optimizer::Let*, std::vector<const optimizer::Variable*>>
        _letRefs;
    optimizer::opt::
        unordered_map<optimizer::ABT::reference_type, optimizer::ABT::reference_type, RefHash>
            _staleDefs;
    // We collect old ABTs in order to avoid the ABA problem.
    std::vector<optimizer::ABT> _staleABTs;

    size_t _inCostlyCtx{0};
    bool _inRefBlock{false};
    bool _changed{false};
};

}  // namespace mongo::stage_builder
