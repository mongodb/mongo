/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <vector>

#include "mongo/db/query/optimizer/algebra/polyvalue.h"

namespace mongo::optimizer {
namespace algebra {

template <typename T, int S>
struct OpNodeStorage {
    T _nodes[S];

    template <typename... Ts>
    OpNodeStorage(Ts&&... vals) : _nodes{std::forward<Ts>(vals)...} {}
};

template <typename T>
struct OpNodeStorage<T, 0> {};

/*=====-----
 *
 * Arity of operator can be:
 * 1. statically known - A, A, A, ...
 * 2. dynamic prefix with optional statically know - vector<A>, A, A, A, ...
 *
 * Denotations map A to some B.
 * So static arity <A,A,A> is mapped to <B,B,B>.
 * Similarly, arity <vector<A>,A> is mapped to <vector<B>,B>
 *
 * There is a wrinkle when B is a reference (if allowed at all)
 * Arity <vector<A>, A, A> is mapped to <vector<B>&, B&, B&> - note that the reference is lifted
 * outside of the vector.
 *
 */
template <typename Slot, typename Derived, int Arity>
class OpSpecificArity : public OpNodeStorage<Slot, Arity> {
    using Base = OpNodeStorage<Slot, Arity>;

public:
    template <typename... Ts>
    OpSpecificArity(Ts&&... vals) : Base({std::forward<Ts>(vals)...}) {
        static_assert(sizeof...(Ts) == Arity, "constructor paramaters do not match");
    }

    template <int I, std::enable_if_t<(I >= 0 && I < Arity), int> = 0>
    auto& get() noexcept {
        return this->_nodes[I];
    }

    template <int I, std::enable_if_t<(I >= 0 && I < Arity), int> = 0>
    const auto& get() const noexcept {
        return this->_nodes[I];
    }
};
/*=====-----
 *
 * Operator with dynamic arity
 *
 */
template <typename Slot, typename Derived, int Arity>
class OpSpecificDynamicArity : public OpSpecificArity<Slot, Derived, Arity> {
    using Base = OpSpecificArity<Slot, Derived, Arity>;

    std::vector<Slot> _dyNodes;

public:
    template <typename... Ts>
    OpSpecificDynamicArity(std::vector<Slot> nodes, Ts&&... vals)
        : Base({std::forward<Ts>(vals)...}), _dyNodes(std::move(nodes)) {}

    auto& nodes() {
        return _dyNodes;
    }
    const auto& nodes() const {
        return _dyNodes;
    }
};

/*=====-----
 *
 * Semantic transport interface
 *
 */
namespace detail {
template <typename D, typename T, typename = std::void_t<>>
struct has_prepare : std::false_type {};
template <typename D, typename T>
struct has_prepare<D, T, std::void_t<decltype(std::declval<D>().prepare(std::declval<T&>()))>>
    : std::true_type {};

template <typename D, typename T>
inline constexpr auto has_prepare_v = has_prepare<D, T>::value;

template <typename Slot, typename Derived, int Arity>
inline constexpr int get_arity(const OpSpecificArity<Slot, Derived, Arity>*) {
    return Arity;
}

template <typename Slot, typename Derived, int Arity>
inline constexpr bool is_dynamic(const OpSpecificArity<Slot, Derived, Arity>*) {
    return false;
}

template <typename Slot, typename Derived, int Arity>
inline constexpr bool is_dynamic(const OpSpecificDynamicArity<Slot, Derived, Arity>*) {
    return true;
}

template <typename T>
using OpConcreteType = typename std::remove_reference_t<T>::template get_t<0>;
}  // namespace detail

template <typename D, bool withSlot>
class OpTransporter {
    D& _domain;

    template <typename T, bool B>
    struct Deducer {};
    template <typename T>
    struct Deducer<T, true> {
        using type = decltype(std::declval<D>().transport(
            std::declval<T>(), std::declval<detail::OpConcreteType<T>&>()));
    };
    template <typename T>
    struct Deducer<T, false> {
        using type =
            decltype(std::declval<D>().transport(std::declval<detail::OpConcreteType<T>&>()));
    };
    template <typename T>
    using deduced_t = typename Deducer<T, withSlot>::type;

    template <typename N, typename T, typename... Ts>
    auto transformStep(N&& slot, T&& op, Ts&&... args) {
        if constexpr (withSlot) {
            return _domain.transport(
                std::forward<N>(slot), std::forward<T>(op), std::forward<Ts>(args)...);
        } else {
            return _domain.transport(std::forward<T>(op), std::forward<Ts>(args)...);
        }
    }

    template <typename N, typename T, size_t... I>
    auto transportUnpack(N&& slot, T&& op, std::index_sequence<I...>) {
        return transformStep(
            std::forward<N>(slot), std::forward<T>(op), op.template get<I>().visit(*this)...);
    }
    template <typename N, typename T, size_t... I>
    auto transportDynamicUnpack(N&& slot, T&& op, std::index_sequence<I...>) {
        std::vector<decltype(slot.visit(*this))> v;
        for (auto& node : op.nodes()) {
            v.emplace_back(node.visit(*this));
        }
        return transformStep(std::forward<N>(slot),
                             std::forward<T>(op),
                             std::move(v),
                             op.template get<I>().visit(*this)...);
    }
    template <typename N, typename T, size_t... I>
    void transportUnpackVoid(N&& slot, T&& op, std::index_sequence<I...>) {
        (op.template get<I>().visit(*this), ...);
        return transformStep(std::forward<N>(slot), std::forward<T>(op), op.template get<I>()...);
    }
    template <typename N, typename T, size_t... I>
    void transportDynamicUnpackVoid(N&& slot, T&& op, std::index_sequence<I...>) {
        for (auto& node : op.nodes()) {
            node.visit(*this);
        }
        (op.template get<I>().visit(*this), ...);
        return transformStep(
            std::forward<N>(slot), std::forward<T>(op), op.nodes(), op.template get<I>()...);
    }

public:
    OpTransporter(D& domain) : _domain(domain) {}

    template <typename N, typename T, typename R = deduced_t<N>>
    R operator()(N&& slot, T&& op) {
        // N is either `PolyValue<Ts...>&` or `const PolyValue<Ts...>&` i.e. reference
        // T is either `A&` or `const A&` where A is one of Ts
        using type = std::remove_reference_t<T>;

        constexpr int arity = detail::get_arity(static_cast<type*>(nullptr));
        constexpr bool is_dynamic = detail::is_dynamic(static_cast<type*>(nullptr));

        if constexpr (detail::has_prepare_v<D, type>) {
            _domain.prepare(std::forward<T>(op));
        }
        if constexpr (is_dynamic) {
            if constexpr (std::is_same_v<R, void>) {
                return transportDynamicUnpackVoid(
                    std::forward<N>(slot), std::forward<T>(op), std::make_index_sequence<arity>{});
            } else {
                return transportDynamicUnpack(
                    std::forward<N>(slot), std::forward<T>(op), std::make_index_sequence<arity>{});
            }
        } else {
            if constexpr (std::is_same_v<R, void>) {
                return transportUnpackVoid(
                    std::forward<N>(slot), std::forward<T>(op), std::make_index_sequence<arity>{});
            } else {
                return transportUnpack(
                    std::forward<N>(slot), std::forward<T>(op), std::make_index_sequence<arity>{});
            }
        }
    }
};

template <typename D, bool withSlot>
class OpWalker {
    D& _domain;

    template <typename N, typename T, typename... Ts>
    auto walkStep(N&& slot, T&& op, Ts&&... args) {
        if constexpr (withSlot) {
            return _domain.walk(
                std::forward<N>(slot), std::forward<T>(op), std::forward<Ts>(args)...);
        } else {
            return _domain.walk(std::forward<T>(op), std::forward<Ts>(args)...);
        }
    }

    template <typename N, typename T, typename... Args, size_t... I>
    auto walkUnpack(N&& slot, T&& op, std::index_sequence<I...>, Args&&... args) {
        return walkStep(std::forward<N>(slot),
                        std::forward<T>(op),
                        std::forward<Args>(args)...,
                        op.template get<I>()...);
    }
    template <typename N, typename T, typename... Args, size_t... I>
    auto walkDynamicUnpack(N&& slot, T&& op, std::index_sequence<I...>, Args&&... args) {
        return walkStep(std::forward<N>(slot),
                        std::forward<T>(op),
                        std::forward<Args>(args)...,
                        op.nodes(),
                        op.template get<I>()...);
    }

public:
    OpWalker(D& domain) : _domain(domain) {}

    template <typename N, typename T, typename... Args>
    auto operator()(N&& slot, T&& op, Args&&... args) {
        // N is either `PolyValue<Ts...>&` or `const PolyValue<Ts...>&` i.e. reference
        // T is either `A&` or `const A&` where A is one of Ts
        using type = std::remove_reference_t<T>;

        constexpr int arity = detail::get_arity(static_cast<type*>(nullptr));
        constexpr bool is_dynamic = detail::is_dynamic(static_cast<type*>(nullptr));

        if constexpr (is_dynamic) {
            return walkDynamicUnpack(std::forward<N>(slot),
                                     std::forward<T>(op),
                                     std::make_index_sequence<arity>{},
                                     std::forward<Args>(args)...);
        } else {
            return walkUnpack(std::forward<N>(slot),
                              std::forward<T>(op),
                              std::make_index_sequence<arity>{},
                              std::forward<Args>(args)...);
        }
    }
};

template <bool withSlot = false, typename D, typename N>
auto transport(N&& node, D& domain) {
    return node.visit(OpTransporter<D, withSlot>{domain});
}

template <bool withSlot = false, typename D, typename N, typename... Args>
auto walk(N&& node, D& domain, Args&&... args) {
    return node.visit(OpWalker<D, withSlot>{domain}, std::forward<Args>(args)...);
}

}  // namespace algebra
}  // namespace mongo::optimizer
