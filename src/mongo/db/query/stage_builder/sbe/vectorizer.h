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

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"
#include "mongo/stdx/unordered_map.h"

#include <list>

namespace mongo::stage_builder {

/**
 * ABT rewriter that constructs an equivalent tree designed to process the data in blocks.
 * Given a suitable ABT tree working on a single vectorized slot and any number of scalar slots,
 * e.g.
 *
 *    if (test(slot1)) then
 *       process1(slot1, scalar1)
 *    else
 *       process2(slot1, scalar2)
 *
 * and a set of vectorizable expressions (test/process1/process2), create a new ABT tree that can
 * process the entire block in slot1 without unpacking it into N scalar values, e.g.
 *
 *    let mask1 = block_test(slot1)
 *    in block_combine(
 *                     block_process1(mask1, slot1, scalar1),
 *                     let mask2 = bitmap_invert(mask1) in block_process2(mask2, slot1, scalar2),
 *                     mask1)
 */
class Vectorizer {
public:
    using VariableTypes =
        stdx::unordered_map<abt::ProjectionName,
                            std::pair<TypeSignature, boost::optional<abt::ProjectionName>>,
                            abt::ProjectionName::Hasher>;
    struct Tree {
        // The ABT expression produced by vectorizing the original expression tree. If not set, the
        // original expression cannot be converted into a vectorized one, usually because of missing
        // functionalities in the engine.
        boost::optional<abt::ABT> expr;
        // The type signature of the expression.
        TypeSignature typeSignature;
        // If set, it points to a slot holding a cell type that must be used in a call to a fold
        // function to obtain the correct result of the evaluation.
        boost::optional<abt::ProjectionName> sourceCell;
    };

    // Declare the context in which the expression will be evaluated, affecting how cell values will
    // be folded at the end of the operation. In a Filter context, operations applied to array
    // values will generate a single boolean result, while, in a Project context, all the results
    // will be packed in a new array result.
    enum class Purpose { Filter, Project };
    Vectorizer(sbe::value::FrameIdGenerator* frameGenerator,
               Purpose purpose,
               bool mayHaveCollation = false)
        : _purpose(purpose), _mayHaveCollation(mayHaveCollation), _frameGenerator(frameGenerator) {}

    // Recursively convert the provided node into a node suitable for vectorized processing.
    // Return a result with an empty 'expr' inside if the node contains an operation that cannot be
    // processed one block at a time.
    // The externalBindings argument contains the known types for the slots referenced by the ABT
    // tree. The externalBitmapSlot argument contains the slot where another stage has already
    // computed a valid selectivity bitmap.
    Tree vectorize(abt::ABT& node,
                   const VariableTypes& externalBindings,
                   boost::optional<SbSlot> externalBitmapSlot);

    // The default visitor for non-supported nodes, returning an empty value to mean "node not
    // supported".
    template <int Arity>
    Tree operator()(const abt::ABT& n, const abt::ABTOpFixedArity<Arity>& op) {
        logUnsupportedConversion(n);
        return {{}, TypeSignature::kAnyScalarType, {}};
    }

    Tree operator()(const abt::ABT& n, const abt::Constant& value);

    Tree operator()(const abt::ABT& n, const abt::Variable& var);

    Tree operator()(const abt::ABT& n, const abt::BinaryOp& op);

    Tree operator()(const abt::ABT& n, const abt::NaryOp& op);

    Tree operator()(const abt::ABT& n, const abt::UnaryOp& op);

    Tree operator()(const abt::ABT& n, const abt::FunctionCall& op);

    Tree operator()(const abt::ABT& n, const abt::Let& op);

    Tree operator()(const abt::ABT& n, const abt::MultiLet& op);

    Tree operator()(const abt::ABT& n, const abt::If& op);

    Tree operator()(const abt::ABT& n, const abt::Switch& op);

private:
    // Helper function that encapsulates the logic to vectorize And/Or statement.
    template <typename Lhs, typename Rhs>
    Tree vectorizeLogicalOp(abt::Operations opType, Lhs lhsNode, Rhs rhsNode);

    // Helper function that encapsulates the logic to vectorize arithmetic operations.
    template <typename Lhs, typename Rhs>
    Tree vectorizeArithmeticOp(abt::Operations opType, Lhs lhsNode, Rhs rhsNode);

    // Helper function that allows the recursive transformation of a N-ary statement.
    Tree vectorizeNaryHelper(const abt::NaryOp& op, size_t argIdx);

    // Helper function that encapsules the logic to vectorize an If statement.
    template <typename Cond, typename Then, typename Else>
    Tree vectorizeCond(Cond condNode, Then thenNode, Else elseNode);

    // Helper function that allows the recursive transformation of a Switch statement.
    Tree vectorizeSwitchHelper(const abt::Switch& op, size_t branchIdx);

    // Ensure that the generated tree is representing a block of measures (i.e.
    // if it's a block expanded from a cell, fold it).
    void foldIfNecessary(Tree& tree, bool useFoldF = false);

    // Return an expression combining all the active bitmap masks currently in scope.
    abt::ABT generateMaskArg();

    abt::ProjectionName generateLocalVarName();

    // Helper method to report unsupported constructs.
    void logUnsupportedConversion(const abt::ABT& node);

    // The purpose of the operations being vectorized.
    Purpose _purpose;

    // Indicates if the current query may have a non-simple collation.
    bool _mayHaveCollation;

    // Keep track of the active mask.
    std::list<abt::ProjectionName> _activeMasks;

    // Keep track of the type of the in-scope variables.
    VariableTypes _variableTypes;

    // Generator for local variable frames.
    sbe::value::FrameIdGenerator* _frameGenerator;
};

}  // namespace mongo::stage_builder
