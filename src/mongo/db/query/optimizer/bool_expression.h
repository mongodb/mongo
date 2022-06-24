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

#include "boost/optional.hpp"
#include <vector>

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

/**
 * Represents a generic boolean expression with arbitrarily nested conjunctions and disjunction
 * elements.
 */
template <class T>
struct BoolExpr {
    class Atom;
    class Conjunction;
    class Disjunction;

    using Node = algebra::PolyValue<Atom, Conjunction, Disjunction>;
    using NodeVector = std::vector<Node>;


    class Atom final : public algebra::OpSpecificArity<Node, 0> {
        using Base = algebra::OpSpecificArity<Node, 0>;

    public:
        Atom(T expr) : Base(), _expr(std::move(expr)) {}

        bool operator==(const Atom& other) const {
            return _expr == other._expr;
        }

        const T& getExpr() const {
            return _expr;
        }
        T& getExpr() {
            return _expr;
        }

    private:
        T _expr;
    };

    class Conjunction final : public algebra::OpSpecificDynamicArity<Node, 0> {
        using Base = algebra::OpSpecificDynamicArity<Node, 0>;

    public:
        Conjunction(NodeVector children) : Base(std::move(children)) {
            uassert(6624351, "Must have at least one child", !Base::nodes().empty());
        }

        bool operator==(const Conjunction& other) const {
            return Base::nodes() == other.nodes();
        }
    };

    class Disjunction final : public algebra::OpSpecificDynamicArity<Node, 0> {
        using Base = algebra::OpSpecificDynamicArity<Node, 0>;

    public:
        Disjunction(NodeVector children) : Base(std::move(children)) {
            uassert(6624301, "Must have at least one child", !Base::nodes().empty());
        }

        bool operator==(const Disjunction& other) const {
            return Base::nodes() == other.nodes();
        }
    };


    /**
     * Utility functions.
     */
    template <typename T1, typename... Args>
    static auto make(Args&&... args) {
        return Node::template make<T1>(std::forward<Args>(args)...);
    }

    template <typename... Args>
    static auto makeSeq(Args&&... args) {
        NodeVector seq;
        (seq.emplace_back(std::forward<Args>(args)), ...);
        return seq;
    }

    template <typename... Args>
    static Node makeSingularDNF(Args&&... args) {
        return make<Disjunction>(
            makeSeq(make<Conjunction>(makeSeq(make<Atom>(T{std::forward<Args>(args)...})))));
    }

    static boost::optional<const T&> getSingularDNF(const Node& n) {
        if (auto disjunction = n.template cast<Disjunction>();
            disjunction != nullptr && disjunction->nodes().size() == 1) {
            if (auto conjunction = disjunction->nodes().front().template cast<Conjunction>();
                conjunction != nullptr && conjunction->nodes().size() == 1) {
                if (auto atom = conjunction->nodes().front().template cast<Atom>();
                    atom != nullptr) {
                    return {atom->getExpr()};
                }
            }
        }
        return {};
    }
};

}  // namespace mongo::optimizer
