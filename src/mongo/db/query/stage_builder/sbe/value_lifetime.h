// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

namespace mongo::stage_builder {

/**
 * Class encapsulating the logic for preserving the lifetime of local values in the SBE VM.
 */
class ValueLifetime {
public:
    /**
     * The possible types of value lifetime that we are interested into tracking.
     * GlobalValue: the value is stored and owned by a slot external to the VM
     * LocalValue: the value is produced by a VM instruction/builtin and it's
     *             going to be released when removed from the VM stack
     * Reference: the value is a non-owned copy of one of the two types above
     */
    enum class ValueType { GlobalValue, LocalValue, Reference };

    ValueLifetime() = default;

    // Recursively walk the provided node and insert the needed operators to ensure that the
    // ownership of the values is properly maintained.
    void validate(abt::ABT& node);

    template <typename T, size_t... I>
    void visitChildren(abt::ABT& n, T&& op, std::index_sequence<I...>) {
        (op.template get<I>().visit(*this), ...);
    }

    // The default visitor.
    template <int Arity>
    ValueType operator()(abt::ABT& n, abt::ABTOpFixedArity<Arity>& op) {
        visitChildren(n, op, std::make_index_sequence<Arity>{});
        return ValueType::LocalValue;
    }

    ValueType operator()(abt::ABT& node, abt::Constant& value);
    ValueType operator()(abt::ABT& n, abt::Variable& var);

    ValueType operator()(abt::ABT& n, abt::LambdaAbstraction& lambda);

    ValueType operator()(abt::ABT& n, abt::Let& let);

    ValueType operator()(abt::ABT& n, abt::MultiLet& multiLet);

    ValueType operator()(abt::ABT& n, abt::BinaryOp& op);

    ValueType operator()(abt::ABT& n, abt::NaryOp& op);

    ValueType operator()(abt::ABT& n, abt::FunctionCall& op);

    ValueType operator()(abt::ABT& n, abt::If& op);

    ValueType operator()(abt::ABT& n, abt::Switch& op);

    bool modified() const {
        return _changed;
    }

private:
    // Replace the reference to a node with a reference to a makeOwn(node) call.
    void wrapNode(abt::ABT& node);

    using VariableTypes =
        stdx::unordered_map<abt::ProjectionName, ValueType, abt::ProjectionName::Hasher>;

    // Helper function that manipulates the tree.
    void swapAndUpdate(abt::ABT& n, abt::ABT newN);

    // Keep track of whether the tree was modified in place.
    bool _changed = false;

    // Keep track of the type of variables.
    VariableTypes _bindings;
};

}  // namespace mongo::stage_builder
