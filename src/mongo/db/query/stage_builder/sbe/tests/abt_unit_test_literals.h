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

#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"

#include <sstream>
#include <string>
#include <typeinfo>

#include <boost/core/demangle.hpp>

namespace mongo::stage_builder::abt_lower::unit_test_abt_literals {
using namespace sbe::value;
using namespace abt;

/**
 * The functions in this file aim to simplify and shorten the manual construction of ABTs for
 * testing. This utility is meant to be used exclusively for tests. It does not necessarily provide
 * an efficient way to construct the tree (e.g. we need to shuffle arguments through a lambda and
 * wrap/unwrap the holders).
 */

template <class Tag>
struct ABTHolder {
    ABT _n;
};

// Strong aliases for expressions.
struct ExprTag {};
using ExprHolder = ABTHolder<ExprTag>;

/**
 * ABT Expressions
 */
template <class T, class T1>
inline T getEnumByName(StringData str, const T1& toStr) {
    for (size_t i = 0; i < sizeof(toStr) / sizeof(toStr[0]); i++) {
        if (str == toStr[i]) {
            return static_cast<T>(i);
        }
    }
    MONGO_UNREACHABLE;
}

inline Operations getOpByName(StringData str) {
    return getEnumByName<Operations>(str, OperationsEnumString::arr_);
}

template <class T>
inline ABTVector holdersToABTs(T holders) {
    ABTVector v;
    for (auto& h : holders) {
        v.push_back(std::move(h._n));
    }
    return v;
}

// String constant.
inline auto operator""_cstr(const char* c, size_t len) {
    return ExprHolder{Constant::str({c, len})};
}

// Int32 constant.
inline auto operator""_cint32(const char* c, size_t len) {
    return ExprHolder{Constant::int32(std::stoi({c, len}))};
}

// Int64 constant.
inline auto operator""_cint64(const char* c, size_t len) {
    return ExprHolder{Constant::int64(std::stol({c, len}))};
}

// Double constant.
inline auto operator""_cdouble(const char* c, size_t len) {
    return ExprHolder{Constant::fromDouble(std::stod({c, len}))};
}

// Null constant.
inline auto _cnull() {
    return ExprHolder{Constant::null()};
}

// Nothing constant.
inline auto _cnothing() {
    return ExprHolder{Constant::nothing()};
}

// NaN constant
inline auto _cNaN() {
    return ExprHolder{Constant::fromDouble(std::numeric_limits<double>::quiet_NaN())};
}

// Boolean constant.
inline auto _cbool(const bool val) {
    return ExprHolder{Constant::boolean(val)};
}

// MinKey constant.
inline auto _cminKey() {
    return ExprHolder{Constant::minKey()};
}

// MaxKey constant.
inline auto _cmaxKey() {
    return ExprHolder{Constant::maxKey()};
}

// Array constant. We expect the arguments to be Constants.
inline auto _carray(auto&&... elements) {
    using namespace sbe::value;

    auto [tag, val] = makeNewArray();
    auto arr = getArrayView(val);
    (arr->push_back(copyValue(elements._n.template cast<Constant>()->get().first,
                              elements._n.template cast<Constant>()->get().second)),
     ...);

    return ExprHolder{make<Constant>(tag, val)};
}

// Empty Array constant.
inline auto _cemparray() {
    return ExprHolder{Constant::emptyArray()};
}

// Empty Object constant.
inline auto _cempobj() {
    return ExprHolder{Constant::emptyObject()};
}

// Variable.
inline auto operator""_var(const char* c, size_t len) {
    return ExprHolder{make<Variable>(ProjectionName{StringData{c, len}})};
}

// Vector of variable names.
template <typename... Ts>
inline auto _varnames(Ts&&... pack) {
    ProjectionNameVector names;
    (names.push_back(std::forward<Ts>(pack)), ...);
    return names;
}

inline auto _unary(StringData name, ExprHolder input) {
    return ExprHolder{make<UnaryOp>(getOpByName(name), std::move(input._n))};
}

inline auto _binary(StringData name, ExprHolder input1, ExprHolder input2) {
    return ExprHolder{
        make<BinaryOp>(getOpByName(name), std::move(input1._n), std::move(input2._n))};
}

template <typename... Ts>
inline auto _nary(StringData name, Ts&&... pack) {
    std::vector<ExprHolder> v;
    (v.push_back(std::forward<Ts>(pack)), ...);
    return ExprHolder{make<NaryOp>(getOpByName(name), holdersToABTs(std::move(v)))};
}

inline auto _if(ExprHolder condExpr, ExprHolder thenExpr, ExprHolder elseExpr) {
    return ExprHolder{
        make<If>(std::move(condExpr._n), std::move(thenExpr._n), std::move(elseExpr._n))};
}

inline auto _switch(ExprHolder condExpr, ExprHolder thenExpr, ExprHolder elseExpr) {
    std::vector<std::pair<ABT, ABT>> conditions = {
        std::make_pair(std::move(condExpr._n), std::move(thenExpr._n))};
    return ExprHolder{make<Switch>(std::move(conditions), std::move(elseExpr._n))};
}

inline auto _switch(ExprHolder condExpr1,
                    ExprHolder thenExpr1,
                    ExprHolder condExpr2,
                    ExprHolder thenExpr2,
                    ExprHolder elseExpr) {
    std::vector<std::pair<ABT, ABT>> conditions = {
        std::make_pair(std::move(condExpr1._n), std::move(thenExpr1._n)),
        std::make_pair(std::move(condExpr2._n), std::move(thenExpr2._n))};
    return ExprHolder{make<Switch>(std::move(conditions), std::move(elseExpr._n))};
}

inline auto _switch(ExprHolder condExpr1,
                    ExprHolder thenExpr1,
                    ExprHolder condExpr2,
                    ExprHolder thenExpr2,
                    ExprHolder condExpr3,
                    ExprHolder thenExpr3,
                    ExprHolder elseExpr) {
    std::vector<std::pair<ABT, ABT>> conditions = {
        std::make_pair(std::move(condExpr1._n), std::move(thenExpr1._n)),
        std::make_pair(std::move(condExpr2._n), std::move(thenExpr2._n)),
        std::make_pair(std::move(condExpr3._n), std::move(thenExpr3._n))};
    return ExprHolder{make<Switch>(std::move(conditions), std::move(elseExpr._n))};
}

inline auto _let(StringData pn, ExprHolder inBind, ExprHolder inExpr) {
    return ExprHolder{make<Let>(ProjectionName{pn}, std::move(inBind._n), std::move(inExpr._n))};
}

inline auto _multiLet(StringData pn1, ExprHolder inBind1, ExprHolder inExpr) {
    return ExprHolder{make<MultiLet>(
        std::vector{std::pair{ProjectionName{pn1}, std::move(inBind1._n)}}, std::move(inExpr._n))};
}

inline auto _multiLet(
    StringData pn1, ExprHolder inBind1, StringData pn2, ExprHolder inBind2, ExprHolder inExpr) {
    return ExprHolder{
        make<MultiLet>(std::vector{std::pair{ProjectionName{pn1}, std::move(inBind1._n)},
                                   std::pair{ProjectionName{pn2}, std::move(inBind2._n)}},
                       std::move(inExpr._n))};
}

inline auto _multiLet(StringData pn1,
                      ExprHolder inBind1,
                      StringData pn2,
                      ExprHolder inBind2,
                      StringData pn3,
                      ExprHolder inBind3,
                      ExprHolder inExpr) {
    return ExprHolder{
        make<MultiLet>(std::vector{std::pair{ProjectionName{pn1}, std::move(inBind1._n)},
                                   std::pair{ProjectionName{pn2}, std::move(inBind2._n)},
                                   std::pair{ProjectionName{pn3}, std::move(inBind3._n)}},
                       std::move(inExpr._n))};
}

inline auto _lambda(StringData pn, ExprHolder body) {
    return ExprHolder{make<LambdaAbstraction>(ProjectionName{pn}, std::move(body._n))};
}

inline auto _lambdaApp(ExprHolder lambda, ExprHolder arg) {
    return ExprHolder{make<LambdaApplication>(std::move(lambda._n), std::move(arg._n))};
}

template <typename... Ts>
inline auto _fn(StringData name, Ts&&... pack) {
    std::vector<ExprHolder> v;
    (v.push_back(std::forward<Ts>(pack)), ...);
    return ExprHolder{make<FunctionCall>(std::string{name}, holdersToABTs(std::move(v)))};
}

/**
 * Shorthand explainer: generate C++ code to construct ABTs in shorthand form. The use case is to
 * provide an easy way to capture an ABT from a JS test and convert it to use in a C++ unit test.
 */
class ExplainInShorthand {
public:
    /**
     * ABT Expressions.
     */
    std::string transport(const Constant& expr) {
        using namespace sbe::value;
        auto getVal = [&]() -> std::string {
            return str::stream{} << "\"" << expr.get() << "\"";
        };

        auto out = str::stream{};
        if (expr.isValueBool()) {
            out << "_cbool(" << (expr.getValueBool() ? "true" : "false") << ")";
        } else if (expr.isArray()) {
            if (getArrayView(expr.get().second)->size() == 0) {
                out << "_cemparray()";
            } else {
                out << "_carray(";
                auto shouldTruncate = true;
                size_t iter = 0;

                if (auto ae = ArrayEnumerator{TypeTags::Array, expr.get().second}; !ae.atEnd()) {
                    while (iter < sbe::PrintOptions::kDefaultArrayObjectOrNestingMaxDepth) {
                        auto getMaker = [](auto&& arg) -> std::string {
                            switch (arg.first) {
                                case TypeTags::NumberDouble:
                                    return (std::stringstream{}
                                            << "sbe::value::bitcastFrom<double>(" << std::showpoint
                                            << arg << ")")
                                        .str();
                                case TypeTags::StringSmall:
                                    return str::stream{} << "sbe::value::makeSmallString(" << arg
                                                         << "_sd).second";
                                default:
                                    return str::stream{} << "unimplemented(" << arg << ")";
                            }
                        };
                        auto aeView = ae.getViewOfValue();
                        out << "std::pair{sbe::value::TypeTags::" << aeView.first << ", "
                            << getMaker(aeView) << "}";
                        ae.advance();
                        if (ae.atEnd()) {
                            shouldTruncate = false;
                            break;
                        }
                        out << ", ";
                        ++iter;
                    }
                    if (shouldTruncate) {
                        out << "...";
                    }
                }
                out << ")";
            }
        } else if (expr.isObject() && getObjectView(expr.get().second)->size() == 0) {
            out << "_cempobj()";
        } else if (expr.isString()) {
            out << expr.get() << "_cstr";
        } else if (expr.isValueInt32()) {
            out << getVal() << "_cint32";
        } else if (expr.isValueInt64()) {
            out << getVal() << "_cint64";
        } else if (expr.isValueDouble()) {
            out << getVal() << "_cdouble";
        } else {
            out << getVal() << "<non-standard constant>";
        }

        return out;
    }

    std::string transport(const Variable& expr) {
        return str::stream() << "\"" << expr.name() << "\""
                             << "_var";
    }

    std::string transport(const UnaryOp& expr, std::string inResult) {
        return str::stream() << "_unary(\"" << toStringData(expr.op()) << "\", " << inResult << ")";
    }

    std::string transport(const BinaryOp& expr, std::string leftResult, std::string rightResult) {
        return str::stream() << "_binary(\"" << toStringData(expr.op()) << "\", " << leftResult
                             << ", " << rightResult << ")";
    }

    template <typename T, typename... Ts>
    std::string transport(const T& node, Ts&&...) {
        return str::stream() << "<transport not implemented for type: '"
                             << boost::core::demangle(typeid(node).name()) << "'>";
    }

    std::string explain(const ABT& n) {
        return algebra::transport<false>(n, *this);
    }
};
}  // namespace mongo::stage_builder::abt_lower::unit_test_abt_literals
