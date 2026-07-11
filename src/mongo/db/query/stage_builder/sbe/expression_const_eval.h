// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/stage_builder/sbe/abt/containers.h"
#include "mongo/db/query/stage_builder/sbe/abt/reference_tracker.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <vector>

namespace mongo {
class CollatorInterface;
}

namespace mongo::stage_builder {

/**
 * 'ExpressionConstEval' is a constant-folding rewrite designed to be used by the SBE stage builder.
 * This class was originally adapted from abt::ConstEval.
 */
class ExpressionConstEval {
public:
    ExpressionConstEval(const CollatorInterface* collator) : _collator(collator) {}

    template <typename T, typename... Ts>
    void transport(abt::ABT&, const T&, Ts&&...) {}

    void transport(abt::ABT& n, const abt::Variable& var);

    void prepare(abt::ABT&, const abt::Let& let);
    void transport(abt::ABT& n, const abt::Let& let, abt::ABT&, abt::ABT& in);
    void prepare(abt::ABT&, const abt::MultiLet& multiLet);
    void transport(abt::ABT& n, const abt::MultiLet& multiLet, std::vector<abt::ABT>& args);
    void prepare(abt::ABT&, const abt::LambdaAbstraction&);
    void transport(abt::ABT&, const abt::LambdaAbstraction&, abt::ABT&);

    void transport(abt::ABT& n, const abt::UnaryOp& op, abt::ABT& child);
    void transport(abt::ABT& n, const abt::BinaryOp& op, abt::ABT& lhs, abt::ABT& rhs);
    void transport(abt::ABT& n, const abt::NaryOp& op, std::vector<abt::ABT>& args);
    void transport(abt::ABT& n, const abt::FunctionCall& op, std::vector<abt::ABT>& args);
    void transport(
        abt::ABT& n, const abt::If& op, abt::ABT& cond, abt::ABT& thenBranch, abt::ABT& elseBranch);
    void transport(abt::ABT& n, const abt::Switch& op, std::vector<abt::ABT>& args);

    void prepare(abt::ABT&, const abt::References& refs);
    void transport(abt::ABT& n, const abt::References& op, std::vector<abt::ABT>&);

    // The tree is passed in as NON-const reference as we will be updating it.
    void optimize(abt::ABT& n);

private:
    struct RefHash {
        size_t operator()(const abt::ABT::reference_type& nodeRef) const {
            return nodeRef.hash();
        }
    };

    void swapAndUpdate(abt::ABT& n, abt::ABT newN);

    abt::DefinitionsMap _variableDefinitions;
    const CollatorInterface* _collator;

    abt::ProjectionNameSet _singleRef;
    abt::opt::unordered_map<abt::ProjectionName, size_t, abt::ProjectionName::Hasher> _varRefs;
    abt::opt::unordered_map<abt::ABT::reference_type, abt::ABT::reference_type, RefHash> _staleDefs;
    // We collect old ABTs in order to avoid the ABA problem.
    std::vector<abt::ABT> _staleABTs;

    size_t _inCostlyCtx{0};
    bool _inRefBlock{false};
    bool _changed{false};
};

}  // namespace mongo::stage_builder
