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

#include <ostream>

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"

namespace mongo::optimizer {

class Constant final : public Operator<Constant, 0>, public ExpressionSyntaxSort {
public:
    Constant(sbe::value::TypeTags tag, sbe::value::Value val);

    static ABT str(std::string str);

    static ABT int32(int32_t valueInt32);
    static ABT int64(int64_t valueInt64);

    static ABT fromDouble(double value);

    static ABT emptyObject();
    static ABT emptyArray();

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

class Variable final : public Operator<Variable, 0>, public ExpressionSyntaxSort {
    std::string _name;

public:
    Variable(std::string inName) : _name(std::move(inName)) {}

    bool operator==(const Variable& other) const {
        return _name == other._name;
    }

    auto& name() const {
        return _name;
    }
};

class UnaryOp final : public Operator<UnaryOp, 1>, public ExpressionSyntaxSort {
    using Base = Operator<UnaryOp, 1>;
    Operations _op;

public:
    UnaryOp(Operations inOp, ABT inExpr) : Base(std::move(inExpr)), _op(inOp) {
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
};

class BinaryOp final : public Operator<BinaryOp, 2>, public ExpressionSyntaxSort {
    using Base = Operator<BinaryOp, 2>;
    Operations _op;

public:
    BinaryOp(Operations inOp, ABT inLhs, ABT inRhs)
        : Base(std::move(inLhs), std::move(inRhs)), _op(inOp) {
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

    const ABT& getRightChild() const {
        return get<1>();
    }
};

class If final : public Operator<If, 3>, public ExpressionSyntaxSort {
    using Base = Operator<If, 3>;

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

    const ABT& getThenChild() const {
        return get<1>();
    }

    const ABT& getElseChild() const {
        return get<2>();
    }
};

class Let final : public Operator<Let, 2>, public ExpressionSyntaxSort {
    using Base = Operator<Let, 2>;

    std::string _varName;

public:
    Let(std::string var, ABT inBind, ABT inExpr)
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

    const ABT& in() const {
        return get<1>();
    }
};

class LambdaAbstraction final : public Operator<LambdaAbstraction, 1>, public ExpressionSyntaxSort {
    using Base = Operator<LambdaAbstraction, 1>;

    std::string _varName;

public:
    LambdaAbstraction(std::string var, ABT inBody)
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

class LambdaApplication final : public Operator<LambdaApplication, 2>, public ExpressionSyntaxSort {
    using Base = Operator<LambdaApplication, 2>;

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

    const ABT& getArgument() const {
        return get<1>();
    }
};

class FunctionCall final : public OperatorDynamicHomogenous<FunctionCall>,
                           public ExpressionSyntaxSort {
    using Base = OperatorDynamicHomogenous<FunctionCall>;
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

class EvalPath final : public Operator<EvalPath, 2>, public ExpressionSyntaxSort {
    using Base = Operator<EvalPath, 2>;

public:
    EvalPath(ABT inPath, ABT inInput) : Base(std::move(inPath), std::move(inInput)) {
        assertPathSort(getPath());
        assertExprSort(getInput());
    }

    bool operator==(const EvalPath& other) const {
        return getPath() == other.getPath() && getInput() == other.getInput();
    }

    const ABT& getPath() const {
        return get<0>();
    }

    ABT& getPath() {
        return get<0>();
    }

    const ABT& getInput() const {
        return get<1>();
    }
};

class EvalFilter final : public Operator<EvalFilter, 2>, public ExpressionSyntaxSort {
    using Base = Operator<EvalFilter, 2>;

public:
    EvalFilter(ABT inPath, ABT inInput) : Base(std::move(inPath), std::move(inInput)) {
        assertPathSort(getPath());
        assertExprSort(getInput());
    }

    bool operator==(const EvalFilter& other) const {
        return getPath() == other.getPath() && getInput() == other.getInput();
    }

    const ABT& getPath() const {
        return get<0>();
    }

    ABT& getPath() {
        return get<0>();
    }

    const ABT& getInput() const {
        return get<1>();
    }
};

/**
 * This class represents a source of values originating from relational nodes.
 */
class Source final : public Operator<Source, 0>, public ExpressionSyntaxSort {
public:
    bool operator==(const Source& other) const {
        return true;
    }
};

}  // namespace mongo::optimizer
