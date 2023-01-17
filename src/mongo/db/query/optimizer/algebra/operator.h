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

#include <cstddef>
#include <utility>
#include <vector>

#include "mongo/util/concepts.h"

namespace mongo::optimizer {
namespace algebra {

/**
 * Concrete storage for 'S' items of type 'T'. This class is an alias for a static array, useful in
 * a tree representation to store a node's children.
 */
template <typename T, int S>
struct OpNodeStorage {
    template <typename... Ts>
    OpNodeStorage(Ts&&... vals) : _nodes{std::forward<Ts>(vals)...} {}

protected:
    T _nodes[S];
};

/**
 * Stub for nodes with no children.
 */
template <typename T>
struct OpNodeStorage<T, 0> {};

/**
 * Nodes which have a fixed arity (number of children) should derive from this class. The 'Slot'
 * determines the generic type to hold for each child.
 */
template <typename Slot, int Arity>
class OpFixedArity : public OpNodeStorage<Slot, Arity> {
    using Base = OpNodeStorage<Slot, Arity>;

public:
    TEMPLATE(typename... Ts)
    REQUIRES(sizeof...(Ts) == Arity)
    OpFixedArity(Ts&&... vals) : Base({std::forward<Ts>(vals)...}) {}

    TEMPLATE(int I)
    REQUIRES(I >= 0 && I < Arity)
    auto& get() noexcept {
        return this->_nodes[I];
    }

    TEMPLATE(int I)
    REQUIRES(I >= 0 && I < Arity)
    const auto& get() const noexcept {
        return this->_nodes[I];
    }
};

/**
 * Nodes which have dynamic arity with an optional minimum number of children.
 */
template <typename Slot, int Arity>
class OpDynamicArity : public OpFixedArity<Slot, Arity> {
    using Base = OpFixedArity<Slot, Arity>;

    std::vector<Slot> _dyNodes;

public:
    template <typename... Ts>
    OpDynamicArity(std::vector<Slot>&& nodes, Ts&&... vals)
        : Base({std::forward<Ts>(vals)...}), _dyNodes(std::move(nodes)) {}

    auto& nodes() {
        return _dyNodes;
    }
    const auto& nodes() const {
        return _dyNodes;
    }
};

/**
 * Semantic transport interface.
 */
namespace detail {
template <typename D, typename T, typename... Args>
using call_prepare_t =
    decltype(std::declval<D>().prepare(std::declval<T&>(), std::declval<Args>()...));

template <typename N, typename D, typename T, typename... Args>
using call_prepare_slot_t = decltype(
    std::declval<D>().prepare(std::declval<N&>(), std::declval<T&>(), std::declval<Args>()...));

template <typename Void, template <class...> class Op, class... Args>
struct has_prepare : std::false_type {};

template <template <class...> class Op, class... Args>
struct has_prepare<std::void_t<Op<Args...>>, Op, Args...> : std::true_type {};

template <bool withSlot, typename N, typename D, typename T, typename... Args>
inline constexpr auto has_prepare_v =
    std::conditional_t<withSlot,
                       has_prepare<void, call_prepare_slot_t, N, D, T, Args...>,
                       has_prepare<void, call_prepare_t, D, T, Args...>>::value;

template <typename Slot, int Arity>
inline constexpr int get_arity(const OpFixedArity<Slot, Arity>*) {
    return Arity;
}

template <typename Slot, int Arity>
inline constexpr bool is_dynamic(const OpFixedArity<Slot, Arity>*) {
    return false;
}

template <typename Slot, int Arity>
inline constexpr bool is_dynamic(const OpDynamicArity<Slot, Arity>*) {
    return true;
}

template <typename T>
using OpConcreteType = typename std::remove_reference_t<T>::template get_t<0>;

}  // namespace detail

/**
 * A transporter is similar to a tree walker that utilizes knowledge of the underlying Operator
 * types to visit each node of an Operator tree in a bottom-up fashion. The Domain class
 * 'D' is used as a callback mechanism by matching the relevant 'transport' overload with
 * the particular node type and children results.
 *
 * The caller may optionally supply 'withSlot' to include a reference to the base PolyValue type as
 * a first argument to the transport callbacks.
 */
template <typename D, bool withSlot>
class OpTransporter {
    D& _domain;

    template <typename T, bool B, typename... Args>
    struct Deducer {};
    template <typename T, typename... Args>
    struct Deducer<T, true, Args...> {
        using type =
            decltype(std::declval<D>().transport(std::declval<T>(),
                                                 std::declval<detail::OpConcreteType<T>&>(),
                                                 std::declval<Args>()...));
    };
    template <typename T, typename... Args>
    struct Deducer<T, false, Args...> {
        using type = decltype(std::declval<D>().transport(
            std::declval<detail::OpConcreteType<T>&>(), std::declval<Args>()...));
    };
    template <typename T, typename... Args>
    using deduced_t = typename Deducer<T, withSlot, Args...>::type;

    template <typename N, typename T, typename... Ts>
    auto transformStep(N&& slot, T&& op, Ts&&... args) {
        if constexpr (withSlot) {
            return _domain.transport(
                std::forward<N>(slot), std::forward<T>(op), std::forward<Ts>(args)...);
        } else {
            return _domain.transport(std::forward<T>(op), std::forward<Ts>(args)...);
        }
    }

    template <typename N, typename T, typename... Args, size_t... I>
    auto transportUnpack(N&& slot, T&& op, std::index_sequence<I...>, Args&&... args) {
        return transformStep(std::forward<N>(slot),
                             std::forward<T>(op),
                             std::forward<Args>(args)...,
                             op.template get<I>().visit(*this, std::forward<Args>(args)...)...);
    }
    template <typename N, typename T, typename... Args, size_t... I>
    auto transportDynamicUnpack(N&& slot, T&& op, std::index_sequence<I...>, Args&&... args) {
        std::vector<decltype(slot.visit(*this, std::forward<Args>(args)...))> v;
        for (auto& node : op.nodes()) {
            v.emplace_back(node.visit(*this, std::forward<Args>(args)...));
        }
        return transformStep(std::forward<N>(slot),
                             std::forward<T>(op),
                             std::forward<Args>(args)...,
                             std::move(v),
                             op.template get<I>().visit(*this, std::forward<Args>(args)...)...);
    }
    template <typename N, typename T, typename... Args, size_t... I>
    void transportUnpackVoid(N&& slot, T&& op, std::index_sequence<I...>, Args&&... args) {
        (op.template get<I>().visit(*this, std::forward<Args>(args)...), ...);
        return transformStep(std::forward<N>(slot),
                             std::forward<T>(op),
                             std::forward<Args>(args)...,
                             op.template get<I>()...);
    }
    template <typename N, typename T, typename... Args, size_t... I>
    void transportDynamicUnpackVoid(N&& slot, T&& op, std::index_sequence<I...>, Args&&... args) {
        for (auto& node : op.nodes()) {
            node.visit(*this, std::forward<Args>(args)...);
        }
        (op.template get<I>().visit(*this, std::forward<Args>(args)...), ...);
        return transformStep(std::forward<N>(slot),
                             std::forward<T>(op),
                             std::forward<Args>(args)...,
                             op.nodes(),
                             op.template get<I>()...);
    }

public:
    OpTransporter(D& domain) : _domain(domain) {}

    template <typename N, typename T, typename... Args, typename R = deduced_t<N, Args...>>
    R operator()(N&& slot, T&& op, Args&&... args) {
        // N is either `PolyValue<Ts...>&` or `const PolyValue<Ts...>&` i.e. reference
        // T is either `A&` or `const A&` where A is one of Ts
        using type = std::remove_reference_t<T>;

        constexpr int arity = detail::get_arity(static_cast<type*>(nullptr));
        constexpr bool is_dynamic = detail::is_dynamic(static_cast<type*>(nullptr));

        if constexpr (detail::has_prepare_v<withSlot, N, D, type, Args...>) {
            if constexpr (withSlot) {
                _domain.prepare(
                    std::forward<N>(slot), std::forward<T>(op), std::forward<Args>(args)...);
            } else {
                _domain.prepare(std::forward<T>(op), std::forward<Args>(args)...);
            }
        }

        if constexpr (is_dynamic) {
            if constexpr (std::is_same_v<R, void>) {
                return transportDynamicUnpackVoid(std::forward<N>(slot),
                                                  std::forward<T>(op),
                                                  std::make_index_sequence<arity>{},
                                                  std::forward<Args>(args)...);
            } else {
                return transportDynamicUnpack(std::forward<N>(slot),
                                              std::forward<T>(op),
                                              std::make_index_sequence<arity>{},
                                              std::forward<Args>(args)...);
            }
        } else {
            if constexpr (std::is_same_v<R, void>) {
                return transportUnpackVoid(std::forward<N>(slot),
                                           std::forward<T>(op),
                                           std::make_index_sequence<arity>{},
                                           std::forward<Args>(args)...);
            } else {
                return transportUnpack(std::forward<N>(slot),
                                       std::forward<T>(op),
                                       std::make_index_sequence<arity>{},
                                       std::forward<Args>(args)...);
            }
        }
    }
};

/**
 * Walker for the Operator* types. Accepts a domain 'D' of 'walk' callback overloads.
 *
 * The caller may optionally supply 'withSlot' to include a reference to base PolyValue as a first
 * argument to the walk callbacks.
 */
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

/**
 * Post-order traversal over the tree given by 'node', with domain D of 'transport' callbacks for
 * each node type. The domain may optionally contain 'prepare' method overloads to pre-visit a node
 * before traversing its children.
 *
 * This method also allows propagating results from the traversal implicitly via the return type of
 * the methods in D. For instance, to return an integer after traversal and a node which has two
 * children, the signature would look something like this:
 *
 *      int transport(const NodeType&, int childResult0, int childResult1)
 *
 */
template <bool withSlot = false, typename D, typename N, typename... Args>
auto transport(N&& node, D& domain, Args&&... args) {
    return node.visit(OpTransporter<D, withSlot>{domain}, std::forward<Args>(args)...);
}

/**
 * Visits 'node' by invoking the appropriate 'walk' overload in domain D. The 'walk' methods should
 * accept the node as the first argument and its children as subsequent arguments with generic type
 * N.
 *
 * Note that this method does not actually traverse the tree given in 'node'; the caller is
 * responsible for manually walking.
 */
template <bool withSlot = false, typename D, typename N, typename... Args>
auto walk(N&& node, D& domain, Args&&... args) {
    return node.visit(OpWalker<D, withSlot>{domain}, std::forward<Args>(args)...);
}

}  // namespace algebra
}  // namespace mongo::optimizer
