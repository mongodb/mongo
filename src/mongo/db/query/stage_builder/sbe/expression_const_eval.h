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

#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/stage_builder/sbe/abt/containers.h"
#include "mongo/db/query/stage_builder/sbe/abt/reference_tracker.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"

#include <cstddef>
#include <vector>

#include <absl/container/node_hash_map.h>

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
    void transport(abt::ABT& n, const abt::LambdaApplication& app, abt::ABT& lam, abt::ABT& arg);
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
