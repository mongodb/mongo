/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/value_lifetime.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
// IWYU pragma: no_include "ext/alloc_traits.h"

namespace mongo::stage_builder {
using namespace std::string_literals;

void ValueLifetime::validate(abt::ABT& node) {
    node.visit(*this);
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& node, abt::Constant& value) {
    // Treat shallow types that don't require lifetime management as if they were
    // global values.
    return sbe::value::isShallowType(value.get().first) ? ValueType::GlobalValue
                                                        : ValueType::LocalValue;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::Variable& var) {
    auto it = _bindings.find(var.name());
    if (it != _bindings.end()) {
        if (it->second == ValueType::LocalValue) {
            return ValueType::Reference;
        }
        return it->second;
    }
    // No explicit type defined, it must be a global slot.
    return ValueType::GlobalValue;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::LambdaAbstraction& lambda) {
    // The Lambda node returns the value of its 'body' child.
    return lambda.getBody().visit(*this);
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::Let& let) {
    // Define the new variable with the type of the 'bind' expression type.
    // If the value is a reference, promote it to local value.
    ValueType bindType = let.bind().visit(*this);
    if (bindType == ValueType::Reference) {
        wrapNode(let.bind());
        bindType = ValueType::LocalValue;
    }
    _bindings[let.varName()] = bindType;

    // The Let node returns the value of its 'in' child.
    ValueType resultType = let.in().visit(*this);

    _bindings.erase(let.varName());

    return resultType;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::MultiLet& multiLet) {
    // Define the new variables with the type of the 'bind' expressions.
    // If the value is a reference, promote it to local value.
    for (size_t idx = 0; idx < multiLet.numBinds(); ++idx) {
        ValueType bindType = multiLet.bind(idx).visit(*this);
        if (bindType == ValueType::Reference) {
            wrapNode(multiLet.bind(idx));
            bindType = ValueType::LocalValue;
        }
        _bindings[multiLet.varName(idx)] = bindType;
    }

    // The MultiLet node returns the value of its 'in' child.
    ValueType resultType = multiLet.in().visit(*this);

    for (auto&& name : multiLet.varNames()) {
        _bindings.erase(name);
    }

    return resultType;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::BinaryOp& op) {

    ValueType lhs = op.getLeftChild().visit(*this);
    ValueType rhs = op.getRightChild().visit(*this);
    switch (op.op()) {
        case abt::Operations::FillEmpty: {
            // FillEmpty propagates either side, promote references to be local values.
            if (lhs == rhs) {
                return lhs;
            } else if (lhs == ValueType::Reference) {
                wrapNode(op.getLeftChild());
            } else if (rhs == ValueType::Reference) {
                wrapNode(op.getRightChild());
            }
            break;
        }

        default:
            break;
    }

    return ValueType::LocalValue;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::NaryOp& op) {
    // Process the arguments, but the logical operation is always going to return a local value.
    for (auto& node : op.nodes()) {
        node.visit(*this);
    }
    return ValueType::LocalValue;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::FunctionCall& op) {
    size_t arity = op.nodes().size();
    if (arity == 3 && (op.name() == "traverseP"s || op.name() == "traverseF"s)) {
        ValueType argType = op.nodes()[0].visit(*this);

        auto lambda = op.nodes()[1].cast<abt::LambdaAbstraction>();
        // Define the lambda variable with the type of the 'bind' expression type.
        _bindings[lambda->varName()] = argType;

        // Process the lambda knowing that its argument will be exactly the type we got from
        // processing the first argument.
        ValueType lambdaType = op.nodes()[1].visit(*this);
        _bindings.erase(lambda->varName());

        // If the first argument is an array, the result is always a local value (array of cloned
        // results for traverseP, a boolean value for traverseF). If it is not an array, then the
        // result is the result of the lambda. Return the most restrictive type.
        return lambdaType == ValueType::Reference ? ValueType::Reference : ValueType::LocalValue;
    }
    std::vector<ValueType> argTypes;
    argTypes.reserve(arity);
    for (auto& node : op.nodes()) {
        argTypes.emplace_back(node.visit(*this));
    }
    if (arity == 2 &&
        (op.name() == "getField"s || op.name() == "getElement"s ||
         op.name() == "getFieldOrElement"s)) {
        // These methods return a local value if the input is a local value, otherwise it returns a
        // reference; when the input is a global value, a reference to a global value can be treated
        // as a global value itself, so we can just propagate the type of the input.
        return argTypes[0];
    }
    if (arity == 3 && op.name() == "fillType"s) {
        // fillType propagates either the input argument or the fallback value, promote references
        // to be local values.
        if (argTypes[0] == argTypes[2]) {
            return argTypes[0];
        } else if (argTypes[0] == ValueType::Reference) {
            wrapNode(op.nodes()[0]);
        } else if (argTypes[2] == ValueType::Reference) {
            wrapNode(op.nodes()[2]);
        }
    }

    return ValueType::LocalValue;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::If& op) {
    op.getCondChild().visit(*this);
    ValueType thenType = op.getThenChild().visit(*this);
    ValueType elseType = op.getElseChild().visit(*this);

    if (thenType == elseType) {
        return thenType;
    } else if (thenType == ValueType::Reference) {
        wrapNode(op.getThenChild());
    } else if (elseType == ValueType::Reference) {
        wrapNode(op.getElseChild());
    }

    return ValueType::LocalValue;
}

ValueLifetime::ValueType ValueLifetime::operator()(abt::ABT& n, abt::Switch& op) {
    std::vector<ValueType> branchTypes;
    branchTypes.reserve(op.getNumBranches() + 1);
    for (size_t i = 0; i < op.getNumBranches(); i++) {
        op.getCondChild(i).visit(*this);
        branchTypes.emplace_back(op.getThenChild(i).visit(*this));
    }
    branchTypes.emplace_back(op.getDefaultChild().visit(*this));

    if (std::all_of(branchTypes.begin(), branchTypes.end(), [&](const ValueType& val) {
            return val == branchTypes[0];
        })) {
        return branchTypes[0];
    } else {
        for (size_t i = 0; i < op.getNumBranches(); i++) {
            if (branchTypes[i] == ValueType::Reference) {
                wrapNode(op.getThenChild(i));
            }
        }
        if (branchTypes.back() == ValueType::Reference) {
            wrapNode(op.getDefaultChild());
        }
    }

    return ValueType::LocalValue;
}

void ValueLifetime::wrapNode(abt::ABT& node) {
    abt::ABTVector arguments;
    arguments.push_back(std::exchange(node, abt::make<abt::Blackhole>()));
    swapAndUpdate(node, abt::make<abt::FunctionCall>("makeOwn", std::move(arguments)));
}

void ValueLifetime::swapAndUpdate(abt::ABT& n, abt::ABT newN) {
    // Do the swap.
    std::swap(n, newN);

    // newN now contains the old ABT and will be destroyed upon exiting this function.

    _changed = true;
}

}  // namespace mongo::stage_builder
