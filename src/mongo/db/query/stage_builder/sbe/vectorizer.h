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

#include <list>

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"
#include "mongo/stdx/unordered_map.h"

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
        stdx::unordered_map<optimizer::ProjectionName,
                            std::pair<TypeSignature, boost::optional<optimizer::ProjectionName>>,
                            optimizer::ProjectionName::Hasher>;
    struct Tree {
        // The ABT expression produced by vectorizing the original expression tree. If not set, the
        // original expression cannot be converted into a vectorized one, usually because of missing
        // functionalities in the engine.
        boost::optional<optimizer::ABT> expr;
        // The type signature of the expression.
        TypeSignature typeSignature;
        // If set, it points to a slot holding a cell type that must be used in a call to a fold
        // function to obtain the correct result of the evaluation.
        boost::optional<optimizer::ProjectionName> sourceCell;
    };

    // Declare the context in which the expression will be evaluated, affecting how cell values will
    // be folded at the end of the operation. In a Filter context, operations applied to array
    // values will generate a single boolean result, while, in a Project context, all the results
    // will be packed in a new array result.
    enum class Purpose { Filter, Project };
    Vectorizer(sbe::value::FrameIdGenerator* frameGenerator, Purpose purpose)
        : _purpose(purpose), _frameGenerator(frameGenerator) {}

    // Recursively convert the provided node into a node suitable for vectorized processing.
    // Return a result with an empty 'expr' inside if the node contains an operation that cannot be
    // processed one block at a time.
    // The externalBindings argument contains the known types for the slots referenced by the ABT
    // tree. The externalBitmapSlot argument contains the slot where another stage has already
    // computed a valid selectivity bitmap.
    Tree vectorize(optimizer::ABT& node,
                   const VariableTypes& externalBindings,
                   boost::optional<sbe::value::SlotId> externalBitmapSlot);

    // The default visitor for non-supported nodes, returning an empty value to mean "node not
    // supported".
    template <int Arity>
    Tree operator()(const optimizer::ABT& n, const optimizer::ABTOpFixedArity<Arity>& op) {
        logUnsupportedConversion(n);
        return {{}, TypeSignature::kAnyScalarType, {}};
    }

    Tree operator()(const optimizer::ABT& n, const optimizer::Constant& value);

    Tree operator()(const optimizer::ABT& n, const optimizer::Variable& var);

    Tree operator()(const optimizer::ABT& n, const optimizer::BinaryOp& op);

    Tree operator()(const optimizer::ABT& n, const optimizer::UnaryOp& op);

    Tree operator()(const optimizer::ABT& n, const optimizer::FunctionCall& op);

    Tree operator()(const optimizer::ABT& n, const optimizer::Let& op);

    Tree operator()(const optimizer::ABT& n, const optimizer::If& op);

private:
    // Ensure that the generated tree is representing a block of measures (i.e.
    // if it's a block expanded from a cell, fold it).
    void foldIfNecessary(Tree& tree, bool useFoldF = false);

    // Return an expression combining all the active bitmap masks currently in scope.
    optimizer::ABT generateMaskArg();

    // Helper method to report unsupported constructs.
    void logUnsupportedConversion(const optimizer::ABT& node);

    // The purpose of the operations being vectorized.
    Purpose _purpose;

    // Keep track of the active mask.
    std::list<optimizer::ProjectionName> _activeMasks;

    // Keep track of the type of the in-scope variables.
    VariableTypes _variableTypes;

    // Generator for local variable frames.
    sbe::value::FrameIdGenerator* _frameGenerator;
};

}  // namespace mongo::stage_builder
