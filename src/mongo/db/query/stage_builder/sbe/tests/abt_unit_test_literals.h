// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/util/modules.h"

#include <sstream>
#include <string>
#include <string_view>
#include <typeinfo>

#include <boost/core/demangle.hpp>

namespace mongo::stage_builder::abt_lower::unit_test_abt_literals {
using namespace std::literals::string_view_literals;
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
inline T getEnumByName(std::string_view str, const T1& toStr) {
    for (size_t i = 0; i < sizeof(toStr) / sizeof(toStr[0]); i++) {
        if (str == toStr[i]) {
            return static_cast<T>(i);
        }
    }
    MONGO_UNREACHABLE;
}

inline Operations getOpByName(std::string_view str) {
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
    (arr->push_back_raw(copyValue(elements._n.template cast<Constant>()->get().tag,
                                  elements._n.template cast<Constant>()->get().value)),
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
    return ExprHolder{make<Variable>(ProjectionName{std::string_view{c, len}})};
}

// Vector of variable names.
template <typename... Ts>
inline auto _varnames(Ts&&... pack) {
    ProjectionNameVector names;
    (names.push_back(std::forward<Ts>(pack)), ...);
    return names;
}

inline auto _unary(std::string_view name, ExprHolder input) {
    return ExprHolder{make<UnaryOp>(getOpByName(name), std::move(input._n))};
}

inline auto _binary(std::string_view name, ExprHolder input1, ExprHolder input2) {
    return ExprHolder{
        make<BinaryOp>(getOpByName(name), std::move(input1._n), std::move(input2._n))};
}

template <typename... Ts>
inline auto _nary(std::string_view name, Ts&&... pack) {
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

inline auto _let(std::string_view pn, ExprHolder inBind, ExprHolder inExpr) {
    return ExprHolder{make<Let>(ProjectionName{pn}, std::move(inBind._n), std::move(inExpr._n))};
}

inline auto _multiLet(std::string_view pn1, ExprHolder inBind1, ExprHolder inExpr) {
    return ExprHolder{make<MultiLet>(
        std::vector{std::pair{ProjectionName{pn1}, std::move(inBind1._n)}}, std::move(inExpr._n))};
}

inline auto _multiLet(std::string_view pn1,
                      ExprHolder inBind1,
                      std::string_view pn2,
                      ExprHolder inBind2,
                      ExprHolder inExpr) {
    return ExprHolder{
        make<MultiLet>(std::vector{std::pair{ProjectionName{pn1}, std::move(inBind1._n)},
                                   std::pair{ProjectionName{pn2}, std::move(inBind2._n)}},
                       std::move(inExpr._n))};
}

inline auto _multiLet(std::string_view pn1,
                      ExprHolder inBind1,
                      std::string_view pn2,
                      ExprHolder inBind2,
                      std::string_view pn3,
                      ExprHolder inBind3,
                      ExprHolder inExpr) {
    return ExprHolder{
        make<MultiLet>(std::vector{std::pair{ProjectionName{pn1}, std::move(inBind1._n)},
                                   std::pair{ProjectionName{pn2}, std::move(inBind2._n)},
                                   std::pair{ProjectionName{pn3}, std::move(inBind3._n)}},
                       std::move(inExpr._n))};
}

inline auto _lambda(std::string_view pn, ExprHolder body) {
    return ExprHolder{make<LambdaAbstraction>(ProjectionName{pn}, std::move(body._n))};
}

template <typename... Ts>
inline auto _fn(std::string_view name, Ts&&... pack) {
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
            if (getArrayView(expr.get().value)->size() == 0) {
                out << "_cemparray()";
            } else {
                out << "_carray(";
                auto shouldTruncate = true;
                size_t iter = 0;

                if (auto ae = ArrayEnumerator{TypeTags::Array, expr.get().value}; !ae.atEnd()) {
                    while (iter < sbe::PrintOptions::kDefaultArrayObjectOrNestingMaxDepth) {
                        auto getMaker = [](auto&& arg) -> std::string {
                            switch (arg.tag) {
                                case TypeTags::NumberDouble:
                                    return (std::stringstream{}
                                            << "sbe::value::bitcastFrom<double>(" << std::showpoint
                                            << arg << ")")
                                        .str();
                                case TypeTags::StringSmall:
                                    return str::stream{} << "sbe::value::makeSmallString(" << arg
                                                         << "sv).second";
                                default:
                                    return str::stream{} << "unimplemented(" << arg << ")";
                            }
                        };
                        auto aeView = ae.getViewOfValue();
                        out << "std::pair{sbe::value::TypeTags::" << aeView.tag << ", "
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
        } else if (expr.isObject() && getObjectView(expr.get().value)->size() == 0) {
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
