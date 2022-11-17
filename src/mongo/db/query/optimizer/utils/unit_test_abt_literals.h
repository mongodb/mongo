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

#include <boost/core/demangle.hpp>
#include <typeinfo>

#include "mongo/db/query/optimizer/node.h"


namespace mongo::optimizer::unit_test_abt_literals {

/**
 * The functions in this file aim to simplify and shorten the manual construction of ABTs for
 * testing. This utility is meant to be used exclusively for tests. It does not necessarily provide
 * an efficient way to construct the tree (e.g. we need to shuffle arguments through a lambda and
 * wrap/unwrap the holders).
 *
 * Note if renaming, changing, etc names of shorthand functions, also update the corresponding
 * 'ExplainInShorthand' transport in order to continue generating valid construction code.
 */

template <class Tag>
struct ABTHolder {
    ABT _n;
};

// Strong aliases for expressions, paths and nodes. Those provide type safety. For example, they
// make it impossible to pass an ABT created as an expression in place of an argument expected to a
// node.
struct ExprTag {};
using ExprHolder = ABTHolder<ExprTag>;
struct PathTag {};
using PathHolder = ABTHolder<PathTag>;
struct NodeTag {};
using NodeHolder = ABTHolder<NodeTag>;


/**
 * ABT Expressions
 */
inline Operations getOpByName(StringData str) {
    for (size_t i = 0; i < sizeof(OperationsEnum::toString) / sizeof(OperationsEnum::toString[0]);
         i++) {
        if (str == OperationsEnum::toString[i]) {
            return static_cast<Operations>(i);
        }
    }
    MONGO_UNREACHABLE;
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
inline auto operator"" _cstr(const char* c, size_t len) {
    return ExprHolder{Constant::str({c, len})};
}

// Int32 constant.
inline auto operator"" _cint32(const char* c, size_t len) {
    return ExprHolder{Constant::int32(std::stoi({c, len}))};
}

// Int64 constant.
inline auto operator"" _cint64(const char* c, size_t len) {
    return ExprHolder{Constant::int64(std::stol({c, len}))};
}

// Double constant.
inline auto operator"" _cdouble(const char* c, size_t len) {
    return ExprHolder{Constant::fromDouble(std::stod({c, len}))};
}

// Variable.
inline auto operator"" _var(const char* c, size_t len) {
    return ExprHolder{make<Variable>(ProjectionName{{c, len}})};
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

inline auto _if(ExprHolder condExpr, ExprHolder thenExpr, ExprHolder elseExpr) {
    return ExprHolder{
        make<If>(std::move(condExpr._n), std::move(thenExpr._n), std::move(elseExpr._n))};
}

inline auto _let(StringData pn, ExprHolder inBind, ExprHolder inExpr) {
    return ExprHolder{make<Let>(ProjectionName{pn}, std::move(inBind._n), std::move(inExpr._n))};
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
    return ExprHolder{make<FunctionCall>(name.toString(), holdersToABTs(std::move(v)))};
}

inline auto _evalp(PathHolder path, ExprHolder input) {
    return ExprHolder{make<EvalPath>(std::move(path._n), std::move(input._n))};
}

inline auto _evalf(PathHolder path, ExprHolder input) {
    return ExprHolder{make<EvalFilter>(std::move(path._n), std::move(input._n))};
}

/**
 * ABT Paths
 */
inline auto _pconst(ExprHolder expr) {
    return PathHolder{make<PathConstant>(std::move(expr._n))};
}

inline auto _plambda(ExprHolder expr) {
    return PathHolder{make<PathLambda>(std::move(expr._n))};
}

inline auto _id() {
    return PathHolder{make<PathIdentity>()};
}

inline auto _default(ExprHolder input) {
    return PathHolder{make<PathDefault>(std::move(input._n))};
}

inline auto _cmp(StringData name, ExprHolder input) {
    return PathHolder{make<PathCompare>(getOpByName(name), std::move(input._n))};
}

template <typename... Ts>
inline auto _drop(Ts&&... pack) {
    FieldNameOrderedSet names;
    (names.emplace(std::forward<Ts>(pack)), ...);
    return PathHolder{make<PathDrop>(std::move(names))};
}

template <typename... Ts>
inline auto _keep(Ts&&... pack) {
    FieldNameOrderedSet names;
    (names.emplace(std::forward<Ts>(pack)), ...);
    return PathHolder{make<PathKeep>(std::move(names))};
}

inline auto _obj() {
    return PathHolder{make<PathObj>()};
}

inline auto _arr() {
    return PathHolder{make<PathArr>()};
}

inline auto _traverse1(PathHolder input) {
    return PathHolder{make<PathTraverse>(std::move(input._n), PathTraverse::kSingleLevel)};
}

inline auto _traverseN(PathHolder input) {
    return PathHolder{make<PathTraverse>(std::move(input._n), PathTraverse::kUnlimited)};
}

inline auto _field(StringData fn, PathHolder input) {
    return PathHolder{make<PathField>(FieldNameType{fn}, std::move(input._n))};
}

inline auto _get(StringData fn, PathHolder input) {
    return PathHolder{make<PathGet>(FieldNameType{fn}, std::move(input._n))};
}

inline auto _composea(PathHolder input1, PathHolder input2) {
    return PathHolder{make<PathComposeA>(std::move(input1._n), std::move(input2._n))};
}

inline auto _composem(PathHolder input1, PathHolder input2) {
    return PathHolder{make<PathComposeM>(std::move(input1._n), std::move(input2._n))};
}

/**
 * ABT Nodes.
 * TODO: add shorthands for all node types if it makes sense.
 */
inline auto _scan(ProjectionName pn, std::string scanDefName) {
    return NodeHolder{make<ScanNode>(std::move(pn), std::move(scanDefName))};
}

inline auto _filter(ExprHolder expr, NodeHolder input) {
    return NodeHolder{make<FilterNode>(std::move(expr._n), std::move(input._n))};
}

inline auto _eval(StringData pn, ExprHolder expr, NodeHolder input) {
    return NodeHolder{
        make<EvaluationNode>(ProjectionName{pn}, std::move(expr._n), std::move(input._n))};
}

inline auto _gb(ProjectionNameVector gbProjNames,
                ProjectionNameVector aggProjNames,
                std::vector<ExprHolder> exprs,
                NodeHolder input) {
    return NodeHolder{make<GroupByNode>(std::move(gbProjNames),
                                        std::move(aggProjNames),
                                        holdersToABTs(std::move(exprs)),
                                        std::move(input._n))};
}

inline auto _union(ProjectionNameVector pns, std::vector<NodeHolder> inputs) {
    return NodeHolder{make<UnionNode>(std::move(pns), holdersToABTs(std::move(inputs)))};
}

/**
 * Note the root returns an ABT instead of a holder.
 */
template <typename... Ts>
inline auto _root(Ts&&... pack) {
    ProjectionNameVector names;
    (names.push_back(std::forward<Ts>(pack)), ...);
    return [n = std::move(names)](NodeHolder input) {
        return make<RootNode>(properties::ProjectionRequirement{n}, std::move(input._n));
    };
}

/**
 * A builder used to construct an ABT. The usage pattern is the following:
 *    1. Construct the builder object.
 *    2. Call appropriate method to add a node of a particular type. The builder works top to
 * bottom: first we add root of the query, then add its child etc until we finalize.
 *    3. At the end call "finish" and pass-in the leaf ABT. This will finalize the builder and
 * return the entire ABT.
 */
class NodeBuilder {
public:
    NodeBuilder() : _rootNode(make<Blackhole>()), _prevChildPtr(&_rootNode) {}

    ABT finish(NodeHolder leaf) {
        invariant(_prevChildPtr);
        *_prevChildPtr = std::move(leaf._n);
        _prevChildPtr = nullptr;
        return std::move(_rootNode);
    }

    NodeBuilder& filter(ExprHolder expr) {
        return advanceChildPtr<FilterNode>(_filter(std::move(expr), makeStub()));
    }

    NodeBuilder& eval(StringData pn, ExprHolder expr) {
        return advanceChildPtr<EvaluationNode>(_eval(std::move(pn), std::move(expr), makeStub()));
    }

    NodeBuilder& gb(ProjectionNameVector gbProjNames,
                    ProjectionNameVector aggProjNames,
                    std::vector<ExprHolder> exprs) {
        return advanceChildPtr<GroupByNode>(
            _gb(std::move(gbProjNames), std::move(aggProjNames), std::move(exprs), makeStub()));
    }

    template <typename... Ts>
    NodeBuilder& root(Ts&&... pack) {
        return advanceChildPtr<RootNode>({_root(std::forward<Ts>(pack)...)(makeStub())});
    }

private:
    NodeHolder makeStub() {
        // Need to use a dummy nullary node here (Blackhole does not work: it is not a node).
        return {make<ValueScanNode>(ProjectionNameVector{}, boost::none)};
    }

    template <class T>
    NodeBuilder& advanceChildPtr(NodeHolder holder) {
        invariant(_prevChildPtr);
        *_prevChildPtr = std::move(holder._n);
        _prevChildPtr = &_prevChildPtr->cast<T>()->getChild();
        return *this;
    }

    // Holds the root node.
    ABT _rootNode;
    // Holds a pointer to the previous node's child.
    ABT* _prevChildPtr;
};

/**
 * Interval expressions.
 */
template <typename... Ts>
inline auto _disj(Ts&&... pack) {
    IntervalReqExpr::NodeVector v;
    (v.push_back(std::forward<Ts>(pack)), ...);
    return IntervalReqExpr::make<IntervalReqExpr::Disjunction>(std::move(v));
}

template <typename... Ts>
inline auto _conj(Ts&&... pack) {
    IntervalReqExpr::NodeVector v;
    (v.push_back(std::forward<Ts>(pack)), ...);
    return IntervalReqExpr::make<IntervalReqExpr::Conjunction>(std::move(v));
}

inline auto _interval(IntervalRequirement req) {
    return IntervalReqExpr::make<IntervalReqExpr::Atom>(std::move(req));
}

inline auto _interval(BoundRequirement low, BoundRequirement high) {
    return _interval({std::move(low), std::move(high)});
}

inline auto _incl(ExprHolder expr) {
    return BoundRequirement(true /*inclusive*/, std::move(expr._n));
}

inline auto _excl(ExprHolder expr) {
    return BoundRequirement(false /*inclusive*/, std::move(expr._n));
}

inline auto _plusInf() {
    return BoundRequirement::makePlusInf();
}

inline auto _minusInf() {
    return BoundRequirement::makeMinusInf();
}

/**
 * Shorthand explainer: generate C++ code to construct ABTs in shorthand form. The use case is to
 * provide an easy way to capture an ABT from a JS test and convert it to use in a C++ unit test.
 */
class ExplainInShorthand {
public:
    ExplainInShorthand(std::string nodeSeparator = "\n")
        : _nodeSeparator(std::move(nodeSeparator)) {}

    /**
     * ABT Expressions.
     */
    std::string transport(const Constant& expr) {
        str::stream out;
        out << "\"" << expr.get() << "\"";

        if (expr.isValueInt32()) {
            out << "_cint32";
        } else if (expr.isValueInt64()) {
            out << "_cint64";
        } else if (expr.isValueDouble()) {
            out << "_cdouble";
        } else if (expr.isString()) {
            out << "_cstr";
        } else {
            out << "<non-standard constant>";
        }

        return out;
    }

    std::string transport(const Variable& expr) {
        return str::stream() << "\"" << expr.name() << "\""
                             << "_var";
    }

    std::string transport(const UnaryOp& expr, std::string inResult) {
        return str::stream() << "_unary(\"" << OperationsEnum::toString[static_cast<int>(expr.op())]
                             << "\", " << inResult << ")";
    }

    std::string transport(const BinaryOp& expr, std::string leftResult, std::string rightResult) {
        return str::stream() << "_binary(\""
                             << OperationsEnum::toString[static_cast<int>(expr.op())] << "\", "
                             << leftResult << ", " << rightResult << ")";
    }

    std::string transport(const EvalPath& expr, std::string pathResult, std::string inputResult) {
        return str::stream() << "_evalp(" << pathResult << ", " << inputResult << ")";
    }

    std::string transport(const EvalFilter& expr, std::string pathResult, std::string inputResult) {
        return str::stream() << "_evalf(" << pathResult << ", " << inputResult << ")";
    }

    /**
     * ABT Paths.
     */
    std::string transport(const PathIdentity& path) {
        return "_id()";
    }

    std::string transport(const PathArr& path) {
        return "_arr()";
    }

    std::string transport(const PathObj& path) {
        return "_obj()";
    }

    std::string transport(const PathCompare& path, std::string valueResult) {
        return str::stream() << "_cmp(\"" << OperationsEnum::toString[static_cast<int>(path.op())]
                             << "\", " << valueResult << ")";
    }

    std::string transport(const PathTraverse& path, std::string inResult) {
        str::stream os;

        if (path.getMaxDepth() == PathTraverse::kSingleLevel) {
            os << "_traverse1";
        } else if (path.getMaxDepth() == PathTraverse::kUnlimited) {
            os << "_traverseN";
        } else {
            os << "<non-standard traverse>";
        }

        return os << "(" << inResult << ")";
    }

    std::string transport(const PathField& path, std::string inResult) {
        return str::stream() << "_field(\"" << path.name() << "\", " << inResult << ")";
    }

    std::string transport(const PathGet& path, std::string inResult) {
        return str::stream() << "_get(\"" << path.name() << "\", " << inResult << ")";
    }

    std::string transport(const PathComposeM& path,
                          std::string leftResult,
                          std::string rightResult) {
        return str::stream() << "_composem(" << leftResult << ", " << rightResult << ")";
    }

    std::string transport(const PathComposeA& path,
                          std::string leftResult,
                          std::string rightResult) {
        return str::stream() << "_composea(" << leftResult << ", " << rightResult << ")";
    }

    /**
     * ABT Nodes.
     */
    std::string transport(const ScanNode& node, std::string /*bindResult*/) {
        return str::stream() << ".finish(_scan(\"" << node.getProjectionName() << "\", \""
                             << node.getScanDefName() << "\"))";
    }

    std::string transport(const FilterNode& node,
                          std::string childResult,
                          std::string filterResult) {
        return str::stream() << ".filter(" << explain(node.getFilter()) << ")" << _nodeSeparator
                             << childResult;
    }

    std::string transport(const EvaluationNode& node,
                          std::string childResult,
                          std::string projResult) {
        return str::stream() << ".eval(\"" << node.getProjectionName() << "\", "
                             << explain(node.getProjection()) << ")" << _nodeSeparator
                             << childResult;
    }

    std::string transport(const GroupByNode& node,
                          std::string childResult,
                          std::string /*bindAggResult*/,
                          std::string /*refsAggResult*/,
                          std::string /*bindGbResult*/,
                          std::string /*refsGbResult*/) {
        str::stream os;
        os << ".gb(_varnames(";
        printProjNames(os, node.getGroupByProjectionNames());
        os << "), _varnames(";
        printProjNames(os, node.getAggregationProjectionNames());
        os << "), {";

        bool first = true;
        for (const ABT& n : node.getAggregationExpressions()) {
            if (first) {
                first = false;
            } else {
                os << ", ";
            }
            os << explain(n);
        }

        return os << "})" << _nodeSeparator << childResult;
    }

    std::string transport(const RootNode& node, std::string childResult, std::string refsResult) {
        str::stream os;
        os << ".root(";
        printProjNames(os, node.getProperty().getProjections().getVector());
        return os << ")" << _nodeSeparator << childResult;
    }

    template <typename T, typename... Ts>
    std::string transport(const T& node, Ts&&...) {
        return str::stream() << "<transport not implemented for type: '"
                             << boost::core::demangle(typeid(node).name()) << "'>";
    }

    std::string explain(const ABT& n) {
        return algebra::transport<false>(n, *this);
    }

private:
    static void printProjNames(str::stream& os, const ProjectionNameVector& v) {
        bool first = true;
        for (const auto& p : v) {
            if (first) {
                first = false;
            } else {
                os << ", ";
            }
            os << "\"" << p << "\"";
        }
    }

    const std::string _nodeSeparator;
};

}  // namespace mongo::optimizer::unit_test_abt_literals
