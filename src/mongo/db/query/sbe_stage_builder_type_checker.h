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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo::stage_builder {

// The signature of a node is the set of all the types that the Value produced at runtime can assume
// (including TypeTags::Nothing).
struct TypeSignature {
    // Return whether this signature is a strict subset of the other signature.
    bool isSubset(TypeSignature other) const {
        return (typesMask & other.typesMask) == typesMask;
    }
    // Return whether this signature shares at least one type with the other signature.
    bool containsAny(TypeSignature other) const {
        return (typesMask & other.typesMask) != 0;
    }
    // Return a new signature containing all the types of this signature plus the ones from the
    // other signature.
    TypeSignature include(TypeSignature other) const {
        return TypeSignature{typesMask | other.typesMask};
    }
    // Return a new signature containing all the types of this signature minus the ones from the
    // other signature.
    TypeSignature exclude(TypeSignature other) const {
        return TypeSignature{typesMask & ~other.typesMask};
    }
    // Return a new signature containing all the types in common between this signature and the
    // other signature.
    TypeSignature intersect(TypeSignature other) const {
        return TypeSignature{typesMask & other.typesMask};
    }

    // Simple bitmask using one bit for each enum in the TypeTags definition.
    int64_t typesMask = 0;
};

/**
 * Class encapsulating the logic for assigning a type signature to the return value of an ABT node.
 */
class TypeChecker {
public:
    // Predefined constants for common types.
    static TypeSignature kAnyType, kArrayType, kBooleanType, kDateTimeType, kNothingType,
        kNumericType, kStringType, kObjectType;

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
    TypeSignature typeCheck(optimizer::ABT& node);

    // Retrieve the type of a variable from the information collected so far.
    TypeSignature getInferredType(optimizer::ProjectionName variable);

    // Associate a type to a variable. If the new type is not a subset of the existing one, throw an
    // error.
    void bind(optimizer::ProjectionName variable, TypeSignature type);

    // Create a local scope where ABT variables can be temporarily assigned to a stricter type
    // definition.
    void enterLocalBinding();
    // Exit from a local scope.
    void exitLocalBinding();

    template <typename T, size_t... I>
    void visitChildren(optimizer::ABT& n, T&& op, std::index_sequence<I...>) {
        (op.template get<I>().visit(*this, false), ...);
    }

    // The default visitor for types we don't have special type checking rules.
    template <int Arity>
    TypeSignature operator()(optimizer::ABT& n,
                             optimizer::ABTOpFixedArity<Arity>& op,
                             bool saveInference) {
        visitChildren(n, op, std::make_index_sequence<Arity>{});
        return kAnyType;
    }

    TypeSignature operator()(optimizer::ABT& node, optimizer::Constant& value, bool saveInference);
    TypeSignature operator()(optimizer::ABT& n, optimizer::Variable& var, bool saveInference);

    TypeSignature operator()(optimizer::ABT& n, optimizer::Let& let, bool saveInference);

    TypeSignature operator()(optimizer::ABT& n, optimizer::UnaryOp& op, bool saveInference);

    TypeSignature operator()(optimizer::ABT& n, optimizer::BinaryOp& op, bool saveInference);

    TypeSignature operator()(optimizer::ABT& n, optimizer::FunctionCall& op, bool saveInference);

    TypeSignature operator()(optimizer::ABT& n, optimizer::If& op, bool saveInference);

    bool modified() const {
        return _changed;
    }

private:
    using VariableTypes = stdx::
        unordered_map<optimizer::ProjectionName, TypeSignature, optimizer::ProjectionName::Hasher>;
    using BindingsType = std::list<VariableTypes>;

    // Helper function that manipulates the tree.
    void swapAndUpdate(optimizer::ABT& n, optimizer::ABT newN);

    // Helper function used to implement isNumber, isString, etc..
    TypeSignature evaluateTypeTest(optimizer::ABT& n,
                                   TypeSignature argSignature,
                                   TypeSignature typeToCheck);

    // Keep track of whether the tree was modified in place.
    bool _changed = false;

    // Keep track of the type of variables at each level of binding.
    BindingsType _bindings;
};

}  // namespace mongo::stage_builder
