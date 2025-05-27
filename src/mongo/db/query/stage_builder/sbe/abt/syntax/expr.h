/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mongo::abt {

/**
 * Marker class for expressions. Mutually exclusive with paths and nodes.
 */
class ExpressionSyntaxSort {};

/**
 * Holds a constant SBE value with corresponding type tag.
 */
class Constant final : public ABTOpFixedArity<0>, public ExpressionSyntaxSort {
public:
    Constant(sbe::value::TypeTags tag, sbe::value::Value val);

    static ABT createFromCopy(sbe::value::TypeTags tag, sbe::value::Value val);

    static ABT str(StringData str);

    static ABT int32(int32_t valueInt32);
    static ABT int64(int64_t valueInt64);

    static ABT fromDouble(double value);

    static ABT fromDecimal(const Decimal128& value);

    static ABT emptyObject();
    static ABT emptyArray();

    static ABT array(auto&&... elements) {
        using namespace sbe::value;
        auto [tag, val] = makeNewArray();
        auto arr = getArrayView(val);
        // Add each {tag, val} pair to the array.
        (arr->push_back(std::forward<decltype(elements)>(elements)), ...);
        return make<Constant>(tag, val);
    }

    static ABT timestamp(const Timestamp& t);
    static ABT date(const Date_t& d);

    static ABT nothing();
    static ABT null();

    static ABT boolean(bool b);

    static ABT minKey();
    static ABT maxKey();

    ~Constant();

    Constant(const Constant& other);
    Constant(Constant&& other) noexcept;

    bool operator==(const Constant& other) const;

    auto get() const {
        return std::pair{_tag, _val};
    }

    bool isString() const;
    StringData getString() const;

    bool isValueInt64() const;
    int64_t getValueInt64() const;

    bool isValueInt32() const;
    int32_t getValueInt32() const;

    bool isValueDouble() const;
    double getValueDouble() const;

    bool isValueDecimal() const;
    Decimal128 getValueDecimal() const;

    bool isValueBool() const;
    bool getValueBool() const;

    bool isNumber() const {
        return sbe::value::isNumber(_tag);
    }

    bool isNothing() const {
        return _tag == sbe::value::TypeTags::Nothing;
    }

    bool isNull() const {
        return _tag == sbe::value::TypeTags::Null;
    }

    bool isObject() const {
        return _tag == sbe::value::TypeTags::Object;
    }

    bool isArray() const {
        return _tag == sbe::value::TypeTags::Array;
    }

private:
    sbe::value::TypeTags _tag;
    sbe::value::Value _val;
};

/**
 * Represents a reference to a binding. The binding is specified by identifier (string). The logic
 * for checking that the reference is valid (e.g., that the referenced binding is in scope) happens
 * elsewhere.
 */
class Variable final : public ABTOpFixedArity<0>, public ExpressionSyntaxSort {
    ProjectionName _name;

public:
    Variable(ProjectionName inName) : _name(std::move(inName)) {}

    bool operator==(const Variable& other) const {
        return _name == other._name;
    }

    auto& name() const {
        return _name;
    }
};

/**
 * Models arithmetic and other operations that accept a single argument, for instance negate.
 */
class UnaryOp final : public ABTOpFixedArity<1>, public ExpressionSyntaxSort {
    using Base = ABTOpFixedArity<1>;
    Operations _op;

public:
    UnaryOp(Operations inOp, ABT inExpr) : Base(std::move(inExpr)), _op(inOp) {
        tassert(6684501, "Unary op expected", isUnaryOp(_op));
        assertExprSort(getChild());
    }

    bool operator==(const UnaryOp& other) const {
        return _op == other._op && getChild() == other.getChild();
    }

    auto op() const {
        return _op;
    }

    const ABT& getChild() const {
        return get<0>();
    }
    ABT& getChild() {
        return get<0>();
    }
};

/**
 * Models arithmetic, comparison, or logical operations that take two arguments, for instance add or
 * subtract.
 */
class BinaryOp final : public ABTOpFixedArity<2>, public ExpressionSyntaxSort {
    using Base = ABTOpFixedArity<2>;
    Operations _op;

public:
    BinaryOp(Operations inOp, ABT inLhs, ABT inRhs)
        : Base(std::move(inLhs), std::move(inRhs)), _op(inOp) {
        tassert(6684502, "Binary op expected", isBinaryOp(_op));
        assertExprSort(getLeftChild());
        assertExprSort(getRightChild());
    }

    bool operator==(const BinaryOp& other) const {
        return _op == other._op && getLeftChild() == other.getLeftChild() &&
            getRightChild() == other.getRightChild();
    }

    auto op() const {
        return _op;
    }

    const ABT& getLeftChild() const {
        return get<0>();
    }

    ABT& getLeftChild() {
        return get<0>();
    }

    const ABT& getRightChild() const {
        return get<1>();
    }

    ABT& getRightChild() {
        return get<1>();
    }
};

/**
 * Models arithmetic or logical operations that can take more than two arguments, for instance add,
 * multiply.
 */
class NaryOp final : public ABTOpDynamicArity<0>, public ExpressionSyntaxSort {
    using Base = ABTOpDynamicArity<0>;
    Operations _op;

public:
    NaryOp(Operations inOp, ABTVector exprs) : Base(std::move(exprs)), _op(inOp) {
        tassert(10199600,
                "operation doesn't allow multiple operands",
                _op == Operations::And || _op == Operations::Or || _op == Operations::Add ||
                    _op == Operations::Mult);
        tassert(10199601, "operation needs at least one operand", nodes().size() >= 1);
        for (auto&& expr : nodes()) {
            assertExprSort(expr);
        }
    }

    bool operator==(const NaryOp& other) const {
        return _op == other._op && nodes() == other.nodes();
    }

    auto op() const {
        return _op;
    }
};

/**
 * Branching operator with a condition expression, "then" expression, and an "else" expression.
 */
class If final : public ABTOpFixedArity<3>, public ExpressionSyntaxSort {
    using Base = ABTOpFixedArity<3>;

public:
    If(ABT inCond, ABT inThen, ABT inElse)
        : Base(std::move(inCond), std::move(inThen), std::move(inElse)) {
        assertExprSort(getCondChild());
        assertExprSort(getThenChild());
        assertExprSort(getElseChild());
    }

    bool operator==(const If& other) const {
        return getCondChild() == other.getCondChild() && getThenChild() == other.getThenChild() &&
            getElseChild() == other.getElseChild();
    }

    const ABT& getCondChild() const {
        return get<0>();
    }

    ABT& getCondChild() {
        return get<0>();
    }

    const ABT& getThenChild() const {
        return get<1>();
    }

    ABT& getThenChild() {
        return get<1>();
    }

    const ABT& getElseChild() const {
        return get<2>();
    }

    ABT& getElseChild() {
        return get<2>();
    }
};

/**
 * Branching operator with multiple condition expressions, one "then" expression for each
 * condition, and a final "else" expression.
 */
class Switch final : public ABTOpDynamicArity<0>, public ExpressionSyntaxSort {
    using Base = ABTOpDynamicArity<0>;

public:
    Switch(ABTVector exprs) : Base(std::move(exprs)) {
        tassert(10130600,
                "switch created with a wrong number of expressions",
                nodes().size() > 1 && ((nodes().size() - 1) % 2) == 0);
        for (auto&& expr : nodes()) {
            assertExprSort(expr);
        }
    }

    Switch(std::vector<std::pair<ABT, ABT>> branchExprs, ABT defaultExpr)
        : Base(std::vector<ABT>()) {
        tassert(10130601, "switch requires at least one condition", !branchExprs.empty());
        nodes().reserve(branchExprs.size() * 2 + 1);
        for (auto&& branch : branchExprs) {
            assertExprSort(branch.first);
            nodes().emplace_back(std::move(branch.first));
            assertExprSort(branch.second);
            nodes().emplace_back(std::move(branch.second));
        }
        assertExprSort(defaultExpr);
        nodes().emplace_back(std::move(defaultExpr));
    }

    bool operator==(const Switch& other) const {
        return nodes() == other.nodes();
    }

    size_t getNumBranches() const {
        return (nodes().size() - 1) / 2;
    }

    const ABT& getCondChild(size_t idx) const {
        tassert(10130602, "branch index out of bounds", idx < getNumBranches());
        return nodes()[idx * 2];
    }

    ABT& getCondChild(size_t idx) {
        tassert(10130603, "branch index out of bounds", idx < getNumBranches());
        return nodes()[idx * 2];
    }

    const ABT& getThenChild(size_t idx) const {
        tassert(10130604, "branch index out of bounds", idx < getNumBranches());
        return nodes()[idx * 2 + 1];
    }

    ABT& getThenChild(size_t idx) {
        tassert(10130605, "branch index out of bounds", idx < getNumBranches());
        return nodes()[idx * 2 + 1];
    }

    const ABT& getDefaultChild() const {
        return nodes().back();
    }

    ABT& getDefaultChild() {
        return nodes().back();
    }
};

/**
 * Defines a variable from one expression and a specified name which is available to be referenced
 * in a second expression.
 */
class Let final : public ABTOpFixedArity<2>, public ExpressionSyntaxSort {
    using Base = ABTOpFixedArity<2>;

    ProjectionName _varName;

public:
    Let(ProjectionName var, ABT inBind, ABT inExpr)
        : Base(std::move(inBind), std::move(inExpr)), _varName(std::move(var)) {
        assertExprSort(bind());
        assertExprSort(in());
    }

    bool operator==(const Let& other) const {
        return _varName == other._varName && bind() == other.bind() && in() == other.in();
    }

    auto& varName() const {
        return _varName;
    }

    const ABT& bind() const {
        return get<0>();
    }

    ABT& bind() {
        return get<0>();
    }


    const ABT& in() const {
        return get<1>();
    }

    ABT& in() {
        return get<1>();
    }
};

/**
 * Defines variables from multiple expressions and specified names which are available to be
 * referenced in a final expression.
 */
class MultiLet final : public ABTOpDynamicArity<0>, public ExpressionSyntaxSort {
    using Base = ABTOpDynamicArity<0>;

    std::vector<ProjectionName> _varNames;

    void validate() {
        tassert(10130800, "Invalid arguments", _varNames.size() + 1 == nodes().size());
        tassert(10130809, "expected at least one binding", !_varNames.empty());

        std::set<ProjectionName> set{_varNames.begin(), _varNames.end()};
        tassert(10130810, "Variable names should be unique", set.size() == _varNames.size());
    }

public:
    MultiLet(std::vector<ProjectionName> names, ABTVector exprs)
        : Base(std::move(exprs)), _varNames(std::move(names)) {
        for (auto&& a : nodes()) {
            assertExprSort(a);
        }
        validate();
    }

    MultiLet(std::vector<std::pair<ProjectionName, ABT>> inBinds, ABT inExpr)
        : Base(std::vector<ABT>()) {
        nodes().reserve(inBinds.size() + 1 /*inExpr*/);
        for (auto&& inBind : inBinds) {
            assertExprSort(inBind.second);
            nodes().emplace_back(std::move(inBind.second));
            _varNames.emplace_back(std::move(inBind.first));
        }
        assertExprSort(inExpr);
        nodes().emplace_back(std::move(inExpr));
        validate();
    }

    bool operator==(const MultiLet& other) const {
        return _varNames == other._varNames && nodes() == other.nodes();
    }

    size_t numBinds() const {
        return _varNames.size();
    }

    auto& varNames() const {
        return _varNames;
    }

    auto& varName(size_t idx) const {
        tassert(10130801, "Index out of bounds", idx < numBinds());
        return _varNames[idx];
    }

    const ABT& bind(size_t idx) const {
        tassert(10130802, "Index out of bounds", idx < numBinds());
        return nodes()[idx];
    }

    ABT& bind(size_t idx) {
        tassert(10130803, "Index out of bounds", idx < numBinds());
        return nodes()[idx];
    }

    const ABT& in() const {
        return nodes().back();
    }

    ABT& in() {
        return nodes().back();
    }
};

/**
 * Represents a single argument lambda. Defines a local variable (representing the argument) which
 * can be referenced within the lambda. The variable takes on the value to which LambdaAbstraction
 * is applied by its parent.
 */
class LambdaAbstraction final : public ABTOpFixedArity<1>, public ExpressionSyntaxSort {
    using Base = ABTOpFixedArity<1>;

    ProjectionName _varName;

public:
    LambdaAbstraction(ProjectionName var, ABT inBody)
        : Base(std::move(inBody)), _varName(std::move(var)) {
        assertExprSort(getBody());
    }

    bool operator==(const LambdaAbstraction& other) const {
        return _varName == other._varName && getBody() == other.getBody();
    }

    auto& varName() const {
        return _varName;
    }

    const ABT& getBody() const {
        return get<0>();
    }

    ABT& getBody() {
        return get<0>();
    }
};

/**
 * Evaluates an expression representing a function over an expression representing the argument to
 * the function.
 */
class LambdaApplication final : public ABTOpFixedArity<2>, public ExpressionSyntaxSort {
    using Base = ABTOpFixedArity<2>;

public:
    LambdaApplication(ABT inLambda, ABT inArgument)
        : Base(std::move(inLambda), std::move(inArgument)) {
        assertExprSort(getLambda());
        assertExprSort(getArgument());
    }

    bool operator==(const LambdaApplication& other) const {
        return getLambda() == other.getLambda() && getArgument() == other.getArgument();
    }

    const ABT& getLambda() const {
        return get<0>();
    }

    ABT& getLambda() {
        return get<0>();
    }

    const ABT& getArgument() const {
        return get<1>();
    }

    ABT& getArgument() {
        return get<1>();
    }
};

/**
 * Dynamic arity operator which passes its children as arguments to a function specified by SBE
 * function expression name.
 */
class FunctionCall final : public ABTOpDynamicArity<0>, public ExpressionSyntaxSort {
    using Base = ABTOpDynamicArity<0>;
    std::string _name;

public:
    FunctionCall(std::string inName, ABTVector inArgs)
        : Base(std::move(inArgs)), _name(std::move(inName)) {
        for (auto& a : nodes()) {
            assertExprSort(a);
        }
    }

    bool operator==(const FunctionCall& other) const {
        return _name == other._name && nodes() == other.nodes();
    }

    auto& name() const {
        return _name;
    }
};

/**
 * Represents a source of values typically from a collection.
 */
class Source final : public ABTOpFixedArity<0>, public ExpressionSyntaxSort {
public:
    bool operator==(const Source& other) const {
        return true;
    }
};

}  // namespace mongo::abt
