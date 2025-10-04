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

#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"
#include "mongo/stdx/unordered_map.h"

#include <cstddef>
#include <list>
#include <utility>

namespace mongo::stage_builder {

/**
 * Class encapsulating the logic for assigning a type signature to the return value of an ABT node.
 */
class TypeChecker {
public:
    TypeChecker();
    TypeChecker(const TypeChecker& parent);

    // Recursively assign a return type to the inputs of the provided node, then try to match them
    // to (one of) the signature of the node. Return the type of the result as defined by the
    // signature of the node.
    // e.g. an Add node has the signatures (Date, Number) -> Date and (Number, Number) -> Number
    //      If the types of the arguments are known, typeCheck would return either Date or Number,
    //      otherwise it would return the union of all the return types in the signature that are
    //      deemed as possible, i.e. Date|Number
    // In case of mismatch, throw an error; in case the node is a type checking function, check if
    // it can be answered on the basis of the type information of its inputs, and replace the node
    // with the one representing its result.
    TypeSignature typeCheck(abt::ABT& node);

    // Retrieve the type of a variable from the information collected so far.
    boost::optional<TypeSignature> getInferredType(abt::ProjectionName variable);

    // Associate a type to a variable. If the new type is not a subset of the existing one, throw an
    // error.
    void bind(abt::ProjectionName variable, TypeSignature type);

    // Create a local scope where ABT variables can be temporarily assigned to a stricter type
    // definition.
    void enterLocalBinding();
    // Exit from a local scope.
    void exitLocalBinding();

    template <typename T, size_t... I>
    void visitChildren(abt::ABT& n, T&& op, std::index_sequence<I...>) {
        (op.template get<I>().visit(*this, false), ...);
    }

    // The default visitor for types we don't have special type checking rules.
    template <int Arity>
    TypeSignature operator()(abt::ABT& n, abt::ABTOpFixedArity<Arity>& op, bool saveInference) {
        visitChildren(n, op, std::make_index_sequence<Arity>{});
        return TypeSignature::kAnyScalarType;
    }

    TypeSignature operator()(abt::ABT& node, abt::Constant& value, bool saveInference);
    TypeSignature operator()(abt::ABT& n, abt::Variable& var, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::LambdaAbstraction& lambda, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::Let& let, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::MultiLet& let, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::UnaryOp& op, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::BinaryOp& op, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::NaryOp& op, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::FunctionCall& op, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::If& op, bool saveInference);

    TypeSignature operator()(abt::ABT& n, abt::Switch& op, bool saveInference);

    bool modified() const {
        return _changed;
    }

private:
    using VariableTypes =
        stdx::unordered_map<abt::ProjectionName, TypeSignature, abt::ProjectionName::Hasher>;
    using BindingsType = std::list<VariableTypes>;

    // Helper function that manipulates the tree.
    void swapAndUpdate(abt::ABT& n, abt::ABT newN);

    // Helper function used to implement isNumber, isString, etc..
    TypeSignature evaluateTypeTest(abt::ABT& n,
                                   TypeSignature argSignature,
                                   TypeSignature typeToCheck);

    // Keep track of whether the tree was modified in place.
    bool _changed = false;

    // Keep track of the type of variables at each level of binding.
    BindingsType _bindings;
};

}  // namespace mongo::stage_builder
