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

#include "mongo/db/query/sbe_stage_builder_vectorizer.h"

#include "mongo/db/query/sbe_stage_builder_abt_helpers.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr.h"

namespace mongo::stage_builder {

Vectorizer::Tree Vectorizer::vectorize(optimizer::ABT& node,
                                       const VariableTypes& externalBindings) {
    _variableTypes = externalBindings;
    auto result = node.visit(*this);
    foldIfNecessary(result);
    return result;
}

void Vectorizer::foldIfNecessary(Tree& tree) {
    if (tree.sourceCell.has_value()) {
        tassert(7946501,
                "Expansion of a cell should generate a block of values",
                TypeSignature::kBlockType.isSubset(tree.typeSignature));
        if (_purpose == Purpose::Filter) {
            tree.expr = makeABTFunction(
                "cellFoldValues_F"_sd, std::move(*tree.expr), makeVariable(*tree.sourceCell));
            // The output of a folding in this case is a block of boolean values.
            tree.typeSignature = TypeSignature::kBlockType.include(TypeSignature::kBooleanType);
        } else {
            tree.expr = makeABTFunction(
                "cellFoldValues_P"_sd, std::move(*tree.expr), makeVariable(*tree.sourceCell));
            // The output of a folding in this case is a block of arrays or single values, so we
            // can't be more precise.
            tree.typeSignature = TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType);
        }
        tree.sourceCell.reset();
    }
}

optimizer::ABT Vectorizer::generateMaskArg() {
    if (_activeMasks.empty()) {
        return optimizer::Constant::nothing();
    }
    boost::optional<optimizer::ABT> tree;
    for (const auto& var : _activeMasks) {
        if (!tree.has_value()) {
            tree = makeVariable(var);
        } else {
            tree = makeABTFunction("valueBlockLogicalAnd"_sd, std::move(*tree), makeVariable(var));
        }
    }
    return std::move(*tree);
}


Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& node,
                                        const optimizer::Constant& value) {
    // A constant can be used as is.
    auto [tag, val] = value.get();
    return {node, getTypeSignature(tag), {}};
}

Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& n, const optimizer::Variable& var) {
    if (auto varIt = _variableTypes.find(var.name()); varIt != _variableTypes.end()) {
        // If the variable holds a cell, extract the block variable from that and propagate the name
        // of the cell variable to the caller to be used when folding back the result.
        if (TypeSignature::kCellType.isSubset(varIt->second)) {
            return {
                makeABTFunction("cellBlockGetFlatValuesBlock"_sd, n),
                varIt->second.exclude(TypeSignature::kCellType).include(TypeSignature::kBlockType),
                var.name()};
        } else {
            return {n, varIt->second, {}};
        }
    }
    return {n, TypeSignature::kAnyScalarType, {}};
}

Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& n, const optimizer::BinaryOp& op) {

    switch (op.op()) {
        case optimizer::Operations::FillEmpty: {
            Tree lhs = op.getLeftChild().visit(*this);
            if (!lhs.expr.has_value()) {
                return lhs;
            }
            Tree rhs = op.getRightChild().visit(*this);
            if (!rhs.expr.has_value()) {
                return rhs;
            }

            // If the argument is a block, and the replacement value is a scalar, create a
            // block-generating operation.
            if (TypeSignature::kBlockType.isSubset(lhs.typeSignature) &&
                !TypeSignature::kBlockType.isSubset(rhs.typeSignature)) {
                return {makeABTFunction(
                            "valueBlockFillEmpty"_sd, std::move(*lhs.expr), std::move(*rhs.expr)),
                        lhs.typeSignature.exclude(TypeSignature::kNothingType)
                            .include(rhs.typeSignature),
                        lhs.sourceCell};
            }
            break;
        }
        case optimizer::Operations::Gt:
        case optimizer::Operations::Gte:
        case optimizer::Operations::Eq:
        case optimizer::Operations::Neq:
        case optimizer::Operations::Lt:
        case optimizer::Operations::Lte: {

            Tree lhs = op.getLeftChild().visit(*this);
            if (!lhs.expr.has_value()) {
                return lhs;
            }
            Tree rhs = op.getRightChild().visit(*this);
            if (!rhs.expr.has_value()) {
                return rhs;
            }

            // If one of the argument is a block, and the other is a scalar value, create a
            // block-generating operation.
            if (TypeSignature::kBlockType.isSubset(lhs.typeSignature) !=
                TypeSignature::kBlockType.isSubset(rhs.typeSignature)) {
                StringData fnName = [&]() {
                    switch (op.op()) {
                        case optimizer::Operations::Gt:
                            return "valueBlockGtScalar"_sd;
                        case optimizer::Operations::Gte:
                            return "valueBlockGteScalar"_sd;
                        case optimizer::Operations::Eq:
                            return "valueBlockEqScalar"_sd;
                        case optimizer::Operations::Neq:
                            return "valueBlockNeqScalar"_sd;
                        case optimizer::Operations::Lt:
                            return "valueBlockLtScalar"_sd;
                        case optimizer::Operations::Lte:
                            return "valueBlockLteScalar"_sd;
                        default:
                            MONGO_UNREACHABLE;
                    }
                }();
                // Propagate the name of the associated cell variable, this is not the place to fold
                // (there could be a fillEmpty node on top of this comparison).
                return {makeABTFunction(fnName, std::move(*lhs.expr), std::move(*rhs.expr)),
                        TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                            .include(lhs.typeSignature.include(rhs.typeSignature)
                                         .intersect(TypeSignature::kNothingType)),
                        TypeSignature::kBlockType.isSubset(lhs.typeSignature) ? lhs.sourceCell
                                                                              : rhs.sourceCell};
            }
            break;
        }
        case optimizer::Operations::And: {
            Tree lhs = op.getLeftChild().visit(*this);
            if (!lhs.expr.has_value()) {
                return lhs;
            }
            // An And operation between two blocks has to work at the level of measures, not on the
            // expanded arrays.
            foldIfNecessary(lhs);

            if (TypeSignature::kBlockType.isSubset(lhs.typeSignature)) {
                // Treat the result of the left side as the mask to be applied on the right side.
                // This way, the right side can decide whether to skip the processing of the indexes
                // where the left side produced a false result.
                auto lhsVar = getABTLocalVariableName(_frameGenerator->generate(), 0);
                _activeMasks.push_back(lhsVar);
                Tree rhs = op.getRightChild().visit(*this);
                _activeMasks.pop_back();
                if (!rhs.expr.has_value()) {
                    return rhs;
                }
                foldIfNecessary(rhs);

                if (TypeSignature::kBlockType.isSubset(rhs.typeSignature)) {
                    return {makeLet(lhsVar,
                                    std::move(*lhs.expr),
                                    makeABTFunction("valueBlockLogicalAnd"_sd,
                                                    makeVariable(lhsVar),
                                                    std::move(*rhs.expr))),
                            TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                                .include(lhs.typeSignature.include(rhs.typeSignature)
                                             .intersect(TypeSignature::kNothingType)),
                            {}};
                }
            }
            break;
        }
        default:
            break;
    }
    return {{}, TypeSignature::kAnyScalarType, {}};
}

Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& n,
                                        const optimizer::FunctionCall& op) {
    size_t arity = op.nodes().size();

    if (op.name() == "traverseF" && arity == 3 && op.nodes()[2].is<optimizer::Constant>() &&
        op.nodes()[2].cast<optimizer::Constant>()->getValueBool() == false) {
        auto argument = op.nodes()[0].visit(*this);
        if (!argument.expr.has_value()) {
            return argument;
        }

        if (TypeSignature::kBlockType.isSubset(argument.typeSignature) &&
            argument.sourceCell.has_value()) {
            // A tree like "traverseF(block_slot, <lambda>, false)" would execute the lambda on the
            // current value in the slot if it is not an array; if it contains an array, it would
            // run the lambda on each element, picking as final result "true" (if at least one of
            // the outputs of the lambda is "true") otherwise "false". This behavior on a cell slot
            // is guaranteed by applying the lambda on the block representing the expanded cell
            // values and then invoking the valueBlockCellFold_F operation on the result.

            const optimizer::LambdaAbstraction* lambda =
                op.nodes()[1].cast<optimizer::LambdaAbstraction>();
            // Reuse the variable name of the lambda so that we don't have to manipulate the code
            // inside the lambda (and to avoid problems if referencing the first argument directly
            // is not side-effect free).
            _variableTypes.insert_or_assign(lambda->varName(), argument.typeSignature);
            auto lambdaArg = lambda->getBody().visit(*this);
            _variableTypes.erase(lambda->varName());
            if (!lambdaArg.expr.has_value()) {
                return lambdaArg;
            }
            return {makeLet(lambda->varName(),
                            std::move(*argument.expr),
                            makeABTFunction("cellFoldValues_F"_sd,
                                            std::move(*lambdaArg.expr),
                                            makeVariable(*argument.sourceCell))),
                    TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                        .include(argument.typeSignature.intersect(TypeSignature::kNothingType)),
                    {}};
        }
    }

    std::vector<Tree> args;
    args.reserve(arity);
    size_t numOfBlockArgs = 0;
    for (size_t i = 0; i < arity; i++) {
        args.emplace_back(op.nodes()[i].visit(*this));
        if (!args.back().expr.has_value()) {
            return {{}, TypeSignature::kAnyScalarType, {}};
        }
        numOfBlockArgs += TypeSignature::kBlockType.isSubset(args.back().typeSignature);
    }
    if (numOfBlockArgs == 0) {
        // This is a pure scalar function, preserve it as it could be used later as an argument for
        // a block-enabled operation.
        optimizer::ABTVector functionArgs;
        functionArgs.reserve(arity);
        for (size_t i = 0; i < arity; i++) {
            functionArgs.emplace_back(std::move(*args[i].expr));
        }
        return {
            makeABTFunction(op.name(), std::move(functionArgs)), TypeSignature::kAnyScalarType, {}};
    }
    if (numOfBlockArgs == 1) {
        // This is a function that doesn't have a block-enabled counterpart, but it is applied to a
        // single block argument; we can support it by adding a loop on the block argument and
        // invoking the function on top of the current scalar value.
        sbe::FrameId frameId = _frameGenerator->generate();
        auto blockArgVar = getABTLocalVariableName(frameId, 0);
        size_t blockArgPos = -1;
        optimizer::ABTVector functionArgs;
        functionArgs.reserve(arity);
        for (size_t i = 0; i < arity; i++) {
            if (TypeSignature::kBlockType.isSubset(args[i].typeSignature)) {
                blockArgPos = i;
                functionArgs.emplace_back(makeVariable(blockArgVar));
            } else {
                functionArgs.emplace_back(std::move(*args[i].expr));
            }
        }
        return {makeABTFunction(
                    "valueBlockApplyLambda"_sd,
                    generateMaskArg(),
                    std::move(*args[blockArgPos].expr),
                    makeLocalLambda(frameId, makeABTFunction(op.name(), std::move(functionArgs)))),
                TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                args[blockArgPos].sourceCell};
    }
    // We don't support this function applied to multiple blocks at the same time.
    return {{}, TypeSignature::kAnyScalarType, {}};
}


}  // namespace mongo::stage_builder
