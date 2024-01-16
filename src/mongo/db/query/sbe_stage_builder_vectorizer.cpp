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

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/sbe_stage_builder_abt_helpers.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {

namespace {

std::string dumpVariables(const Vectorizer::VariableTypes& variableTypes) {
    std::ostringstream os;
    for (const auto& var : variableTypes) {
        os << var.first.value() << ": "
           << (TypeSignature::kBlockType.isSubset(var.second.first) ? "block of " : "")
           << (TypeSignature::kCellType.isSubset(var.second.first) ? "cell of " : "");
        if (TypeSignature::kAnyScalarType.exclude(var.second.first).isEmpty()) {
            os << "anything";
        } else {
            std::string separator = "";
            for (auto varType : getBSONTypesFromSignature(var.second.first)) {
                os << separator << varType;
                separator = "|";
            }
        }
        os << std::endl;
    }
    return os.str();
}

}  // namespace

Vectorizer::Tree Vectorizer::vectorize(optimizer::ABT& node,
                                       const VariableTypes& externalBindings,
                                       boost::optional<sbe::value::SlotId> externalBitmapSlot) {
    _variableTypes = externalBindings;
    if (externalBitmapSlot) {
        _activeMasks.push_back(getABTVariableName(*externalBitmapSlot));
    }
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

void Vectorizer::logUnsupportedConversion(const optimizer::ABT& node) {
    LOGV2_DEBUG_OPTIONS(8141600,
                        2,
                        {logv2::LogTruncation::Disabled},
                        "Operation is not supported in block-oriented mode",
                        "node"_attr = optimizer::ExplainGenerator::explainV2(node),
                        "variables"_attr = dumpVariables(_variableTypes));
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
        if (TypeSignature::kCellType.isSubset(varIt->second.first)) {
            Tree result = Tree{makeABTFunction("cellBlockGetFlatValuesBlock"_sd, n),
                               varIt->second.first.exclude(TypeSignature::kCellType)
                                   .include(TypeSignature::kBlockType),
                               var.name()};
            if (_purpose == Purpose::Project) {
                // When we are computing projections, we always work on folded values.
                foldIfNecessary(result);
            }
            return result;
        } else {
            return {n, varIt->second.first, varIt->second.second};
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

            // If the argument is a block, create a block-generating operation.
            if (TypeSignature::kBlockType.isSubset(lhs.typeSignature)) {
                return {makeABTFunction(TypeSignature::kBlockType.isSubset(rhs.typeSignature)
                                            ? "valueBlockFillEmptyBlock"_sd
                                            : "valueBlockFillEmpty"_sd,
                                        std::move(*lhs.expr),
                                        std::move(*rhs.expr)),
                        lhs.typeSignature.exclude(TypeSignature::kNothingType)
                            .include(rhs.typeSignature),
                        lhs.sourceCell};
            } else {
                // Preserve scalar operation.
                return {
                    make<optimizer::BinaryOp>(op.op(), std::move(*lhs.expr), std::move(*rhs.expr)),
                    lhs.typeSignature.exclude(TypeSignature::kNothingType)
                        .include(rhs.typeSignature),
                    {}};
            }
            break;
        }
        case optimizer::Operations::Cmp3w: {
            Tree lhs = op.getLeftChild().visit(*this);
            if (!lhs.expr.has_value()) {
                return lhs;
            }
            Tree rhs = op.getRightChild().visit(*this);
            if (!rhs.expr.has_value()) {
                return rhs;
            }

            // If the left argument is a block, and the right is a scalar value, create a
            // block-generating operation.
            if (TypeSignature::kBlockType.isSubset(lhs.typeSignature)) {
                if (!TypeSignature::kBlockType.isSubset(rhs.typeSignature)) {

                    // Propagate the name of the associated cell variable, this is not the place to
                    // fold (there could be a fillEmpty node on top of this comparison).
                    return {makeABTFunction("valueBlockCmp3wScalar"_sd,
                                            std::move(*lhs.expr),
                                            std::move(*rhs.expr)),
                            TypeSignature::kBlockType
                                .include(getTypeSignature(sbe::value::TypeTags::NumberInt32))
                                .include(lhs.typeSignature.include(rhs.typeSignature)
                                             .intersect(TypeSignature::kNothingType)),
                            lhs.sourceCell};
                }
            } else {
                // Preserve scalar operation.
                return {
                    make<optimizer::BinaryOp>(op.op(), std::move(*lhs.expr), std::move(*rhs.expr)),
                    getTypeSignature(sbe::value::TypeTags::NumberInt32)
                        .include(lhs.typeSignature.include(rhs.typeSignature)
                                     .intersect(TypeSignature::kNothingType)),
                    {}};
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
            if (TypeSignature::kBlockType.isSubset(lhs.typeSignature)) {
                if (!TypeSignature::kBlockType.isSubset(rhs.typeSignature)) {
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
                    // Propagate the name of the associated cell variable, this is not the place to
                    // fold (there could be a fillEmpty node on top of this comparison).
                    return {makeABTFunction(fnName, std::move(*lhs.expr), std::move(*rhs.expr)),
                            TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                                .include(lhs.typeSignature.include(rhs.typeSignature)
                                             .intersect(TypeSignature::kNothingType)),
                            TypeSignature::kBlockType.isSubset(lhs.typeSignature) ? lhs.sourceCell
                                                                                  : rhs.sourceCell};
                }
            } else {
                // Preserve scalar operation.
                return {
                    make<optimizer::BinaryOp>(op.op(), std::move(*lhs.expr), std::move(*rhs.expr)),
                    TypeSignature::kBooleanType.include(
                        lhs.typeSignature.include(rhs.typeSignature)
                            .intersect(TypeSignature::kNothingType)),
                    {}};
            }
            break;
        }
        case optimizer::Operations::And:
        case optimizer::Operations::Or: {
            Tree lhs = op.getLeftChild().visit(*this);
            if (!lhs.expr.has_value()) {
                return lhs;
            }
            // An And/Or operation between two blocks has to work at the level of measures, not on
            // the expanded arrays.
            foldIfNecessary(lhs);

            if (TypeSignature::kBlockType.isSubset(lhs.typeSignature)) {
                // Treat the result of the left side as the mask to be applied on the right side.
                // This way, the right side can decide whether to skip the processing of the indexes
                // where the left side produced a false result.
                auto lhsVar = getABTLocalVariableName(_frameGenerator->generate(), 0);

                auto mask = op.op() == optimizer::Operations::And
                    ? lhsVar
                    : getABTLocalVariableName(_frameGenerator->generate(), 0);

                _activeMasks.push_back(mask);
                Tree rhs = op.getRightChild().visit(*this);
                _activeMasks.pop_back();
                if (!rhs.expr.has_value()) {
                    return rhs;
                }
                foldIfNecessary(rhs);

                if (TypeSignature::kBlockType.isSubset(rhs.typeSignature)) {
                    return {op.op() == optimizer::Operations::And
                                ? makeLet(lhsVar,
                                          std::move(*lhs.expr),
                                          makeABTFunction("valueBlockLogicalAnd"_sd,
                                                          makeVariable(lhsVar),
                                                          std::move(*rhs.expr)))
                                : makeLet(lhsVar,
                                          std::move(*lhs.expr),
                                          makeLet(mask,
                                                  makeABTFunction(
                                                      "valueBlockLogicalNot",
                                                      makeABTFunction(
                                                          "valueBlockFillEmpty"_sd,
                                                          makeVariable(lhsVar),
                                                          optimizer::Constant::boolean(false))),
                                                  makeABTFunction("valueBlockLogicalOr"_sd,
                                                                  makeVariable(lhsVar),
                                                                  std::move(*rhs.expr)))),
                            TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                                .include(lhs.typeSignature.include(rhs.typeSignature)
                                             .intersect(TypeSignature::kNothingType)),
                            {}};
                }
            } else {
                Tree rhs = op.getRightChild().visit(*this);
                if (!rhs.expr.has_value()) {
                    return rhs;
                }
                // Preserve scalar operation.
                return {
                    make<optimizer::BinaryOp>(op.op(), std::move(*lhs.expr), std::move(*rhs.expr)),
                    TypeSignature::kBooleanType.include(
                        lhs.typeSignature.include(rhs.typeSignature)
                            .intersect(TypeSignature::kNothingType)),
                    {}};
            }
            break;
        }
        default:
            break;
    }
    logUnsupportedConversion(n);
    return {{}, TypeSignature::kAnyScalarType, {}};
}

Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& n, const optimizer::UnaryOp& op) {
    Tree operand = op.getChild().visit(*this);
    if (!operand.expr.has_value()) {
        return operand;
    }
    if (!TypeSignature::kBlockType.isSubset(operand.typeSignature)) {
        return {makeUnaryOp(op.op(), std::move(*operand.expr)),
                operand.typeSignature,
                operand.sourceCell};
    }
    switch (op.op()) {
        case optimizer::Operations::Not: {
            return {makeABTFunction("valueBlockLogicalNot"_sd, std::move(*operand.expr)),
                    TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                        .include(operand.typeSignature.intersect(TypeSignature::kNothingType)),
                    operand.sourceCell};
            break;
        }
        default:
            break;
    }
    logUnsupportedConversion(n);
    return {{}, TypeSignature::kAnyScalarType, {}};
}

Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& n,
                                        const optimizer::FunctionCall& op) {
    size_t arity = op.nodes().size();

    if (op.name() == "blockTraverseFPlaceholder" && arity == 2) {
        // This placeholder function is injected when a tree like "traverseF(block_slot, <lambda>,
        // false)" would be used on scalar values. The traverseF would execute the lambda on the
        // current value in the slot if it is not an array; if it contains an array, it would
        // run the lambda on each element, picking as final result "true" (if at least one of
        // the outputs of the lambda is "true") otherwise "false". This behavior on a cell slot
        // is guaranteed by applying the lambda on the block representing the expanded cell
        // values and then invoking the valueBlockCellFold_F operation on the result.

        auto argument = op.nodes()[0].visit(*this);
        if (!argument.expr.has_value()) {
            return argument;
        }

        if (TypeSignature::kBlockType.isSubset(argument.typeSignature) &&
            argument.sourceCell.has_value()) {
            const optimizer::LambdaAbstraction* lambda =
                op.nodes()[1].cast<optimizer::LambdaAbstraction>();
            // Reuse the variable name of the lambda so that we don't have to manipulate the code
            // inside the lambda (and to avoid problems if the expression we are going to iterate
            // over has side effects and the lambda references it multiple times, as replacing it
            // directly in code would imply executing more than once). Don't propagate the reference
            // to the cell slot, as we are going to fold the result and we don't want that the
            // lambda does it too.
            _variableTypes.insert_or_assign(lambda->varName(),
                                            std::make_pair(argument.typeSignature, boost::none));
            auto lambdaArg = lambda->getBody().visit(*this);
            _variableTypes.erase(lambda->varName());
            if (!lambdaArg.expr.has_value()) {
                return lambdaArg;
            }
            // If the body of the lambda is just a scalar constant, create a block
            // of the same size of the block argument filled with that value.
            if (!TypeSignature::kBlockType.isSubset(lambdaArg.typeSignature)) {
                lambdaArg.expr = makeABTFunction(
                    "valueBlockNewFill"_sd,
                    std::move(*lambdaArg.expr),
                    makeABTFunction("valueBlockSize"_sd, makeVariable(lambda->varName())));
                lambdaArg.typeSignature =
                    TypeSignature::kBlockType.include(lambdaArg.typeSignature);
                lambdaArg.sourceCell = boost::none;
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
    if (op.name() == "exists" && arity == 1) {
        auto argument = op.nodes()[0].visit(*this);
        if (!argument.expr.has_value()) {
            return argument;
        }

        if (TypeSignature::kBlockType.isSubset(argument.typeSignature)) {
            return {makeABTFunction("valueBlockExists"_sd, std::move(*argument.expr)),
                    TypeSignature::kBlockType.include(TypeSignature::kBooleanType),
                    argument.sourceCell};
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

    // We don't support this function applied to multiple blocks at the same time.
    logUnsupportedConversion(n);
    return {{}, TypeSignature::kAnyScalarType, {}};
}

Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& n, const optimizer::Let& op) {
    // Simply recreate the Let node using the processed inputs.
    auto bind = op.bind().visit(*this);
    if (!bind.expr.has_value()) {
        return bind;
    }
    // Forward the inferred type to the inner expression.
    _variableTypes.insert_or_assign(op.varName(),
                                    std::make_pair(bind.typeSignature, bind.sourceCell));
    auto body = op.in().visit(*this);
    _variableTypes.erase(op.varName());
    if (!body.expr.has_value()) {
        return body;
    }
    return {makeLet(op.varName(), std::move(*bind.expr), std::move(*body.expr)),
            body.typeSignature,
            body.sourceCell};
}

Vectorizer::Tree Vectorizer::operator()(const optimizer::ABT& n, const optimizer::If& op) {
    auto test = op.getCondChild().visit(*this);
    if (!test.expr.has_value()) {
        return test;
    }
    foldIfNecessary(test);

    if (TypeSignature::kBlockType.isSubset(test.typeSignature)) {
        auto blockify = [](Tree& tree, optimizer::ProjectionName var) {
            if (!TypeSignature::kBlockType.isSubset(tree.typeSignature)) {
                tree.expr =
                    makeABTFunction("valueBlockNewFill"_sd,
                                    makeIf(makeABTFunction("valueBlockNone"_sd,
                                                           makeVariable(var),
                                                           optimizer::Constant::boolean(true)),
                                           optimizer::Constant::nothing(),
                                           std::move(*tree.expr)),
                                    makeABTFunction("valueBlockSize"_sd, makeVariable(var)));
                tree.typeSignature = TypeSignature::kBlockType.include(tree.typeSignature);
                tree.sourceCell = boost::none;
            }
        };

        // Treat the result of the condition as the mask to be applied on the 'then' side, and its
        // flipped representation as the mask for the 'else' branch.
        auto lhsVar = getABTLocalVariableName(_frameGenerator->generate(), 0);
        _activeMasks.push_back(lhsVar);
        auto thenBranch = op.getThenChild().visit(*this);
        _activeMasks.pop_back();
        if (!thenBranch.expr.has_value()) {
            return thenBranch;
        }
        // If the branch produces a scalar value, blockify it.
        blockify(thenBranch, lhsVar);

        auto rhsVar = getABTLocalVariableName(_frameGenerator->generate(), 0);
        _activeMasks.push_back(rhsVar);
        auto elseBranch = op.getElseChild().visit(*this);
        _activeMasks.pop_back();
        if (!elseBranch.expr.has_value()) {
            return elseBranch;
        }
        // If the branch produces a scalar value, blockify it.
        blockify(elseBranch, rhsVar);

        boost::optional<optimizer::ProjectionName> sameCell = thenBranch.sourceCell.has_value() &&
                elseBranch.sourceCell.has_value() &&
                *thenBranch.sourceCell == *elseBranch.sourceCell
            ? thenBranch.sourceCell
            : boost::none;
        // If we can't identify a single cell for both branches, fold them.
        if (!sameCell.has_value()) {
            foldIfNecessary(thenBranch);
            foldIfNecessary(elseBranch);
        }
        return {makeLet(lhsVar,
                        std::move(*test.expr),
                        makeABTFunction("valueBlockCombine"_sd,
                                        std::move(*thenBranch.expr),
                                        makeLet(rhsVar,
                                                makeABTFunction("valueBlockLogicalNot"_sd,
                                                                makeVariable(lhsVar)),
                                                std::move(*elseBranch.expr)),
                                        makeVariable(lhsVar))),
                thenBranch.typeSignature.include(elseBranch.typeSignature),
                sameCell};

    } else {
        // Scalar test, keep it as it is.
        auto thenBranch = op.getThenChild().visit(*this);
        if (!thenBranch.expr.has_value()) {
            return thenBranch;
        }
        auto elseBranch = op.getElseChild().visit(*this);
        if (!elseBranch.expr.has_value()) {
            return elseBranch;
        }
        if (TypeSignature::kBlockType.isSubset(thenBranch.typeSignature) !=
            TypeSignature::kBlockType.isSubset(elseBranch.typeSignature)) {
            // TODO: blockify the scalar branch.
            return {{}, TypeSignature::kAnyScalarType, {}};
        }
        boost::optional<optimizer::ProjectionName> sameCell;
        if (TypeSignature::kBlockType.isSubset(thenBranch.typeSignature)) {
            sameCell = thenBranch.sourceCell.has_value() && elseBranch.sourceCell.has_value() &&
                    *thenBranch.sourceCell != *elseBranch.sourceCell
                ? thenBranch.sourceCell
                : boost::none;
            // If we can't identify a single cell for both branches, fold them.
            if (!sameCell.has_value()) {
                foldIfNecessary(thenBranch);
                foldIfNecessary(elseBranch);
            }
        }
        return {
            makeIf(std::move(*test.expr), std::move(*thenBranch.expr), std::move(*elseBranch.expr)),
            thenBranch.typeSignature.include(elseBranch.typeSignature),
            sameCell};
    }
}

}  // namespace mongo::stage_builder
