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

#include "mongo/db/query/algebra/polyvalue.h"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <boost/container/vector.hpp>

namespace mongo::algebra {

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

// Forward declaration to allow friend declaration.
namespace detail {
template <typename Tree, typename Ref>
requires(!std::is_reference_v<Tree>)  //
class TreeCursor;
}  // namespace detail

/**
 * Nodes which have a fixed arity (number of children) should derive from this class. The 'Slot'
 * determines the generic type to hold for each child.
 */
template <typename Slot, int Arity>
class OpFixedArity : public OpNodeStorage<Slot, Arity> {
    using Base = OpNodeStorage<Slot, Arity>;

    template <typename Tree, typename Ref>
    requires(!std::is_reference_v<Tree>)  //
    friend class detail::TreeCursor;

public:
    template <typename... Ts>
    requires(sizeof...(Ts) == Arity)
    OpFixedArity(Ts&&... vals) : Base({std::forward<Ts>(vals)...}) {}

    template <int I>
    requires(I >= 0 && I < Arity)
    auto& get() {
        return this->_nodes[I];
    }

    template <int I>
    requires(I >= 0 && I < Arity)
    const auto& get() const {
        return this->_nodes[I];
    }

protected:
    /**
     * Helper function to compare 2 nodes children with each other. Returns true if all children
     * with matching positions are equal and false otherwise.
     */
    bool allChildrenEqual(const OpFixedArity<Slot, Arity>& other) const {
        for (int i = 0; i < Arity; ++i) {
            if (this->_nodes[i] != other._nodes[i]) {
                return false;
            }
        }
        return true;
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
using call_prepare_slot_t = decltype(std::declval<D>().prepare(
    std::declval<N&>(), std::declval<T&>(), std::declval<Args>()...));

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

/**
 * Determines the type of the first case of a given PolyValue.
 *
 * For example, given PolyValue<Leaf, BinaryNode>, the concrete type would be Leaf.
 */
template <typename T>
using OpConcreteType = typename std::remove_reference_t<T>::template get_t<0>;

// Implements 'deduced_t'.
template <typename D, bool withSlot, typename T, typename... Args>
struct Deducer {};

template <typename D, typename T, typename... Args>
struct Deducer<D, true, T, Args...> {
    using type = decltype(std::declval<D>().transport(
        std::declval<T>(), std::declval<detail::OpConcreteType<T>&>(), std::declval<Args>()...));
};
template <typename D, typename T, typename... Args>
struct Deducer<D, false, T, Args...> {
    using type = decltype(std::declval<D>().transport(std::declval<detail::OpConcreteType<T>&>(),
                                                      std::declval<Args>()...));
};

/**
 * Determines the return type of the D::transport() handlers.
 *
 * Specifically, checks the return type of whichever transport() overload handles the first case
 * of the PolyValue. We assume this first case has zero children.
 *
 * 'T' is the type being visited: PolyValue, or PolyValue::reference_type, or a (possibly const)
 * reference to either of those. When 'withSlot' is true, transport() overloads expect this to
 * be forwarded as the first argument.
 *
 * 'Args...' is a pack of types of any extra arguments the transport() overloads are expecting,
 * forwarded from the top-level algebra::transport call.
 */
template <typename D, bool withSlot, typename T, typename... Args>
using deduced_t = typename Deducer<D, withSlot, T, Args...>::type;

template <size_t... I>
auto unpackDynamic(std::index_sequence<I...>, auto&& op, auto&& cb) {
    return cb(op.nodes(), op.template get<I>()...);
}

template <size_t... I>
auto unpackFixed(std::index_sequence<I...>, auto&& op, auto&& cb) {
    return cb(op.template get<I>()...);
}

/**
 * Helper for unpacking the children of an op.
 *
 * Calls the callback with:
 *   - the vector of dynamic children, if it's a dynamic-arity op.
 *   - the static children as separate arguments.
 */
template <typename Op>
auto unpack(Op& op, auto&& cb) {
    // N is either `PolyValue<Ts...>&` or `const PolyValue<Ts...>&` i.e. reference
    // T is either `A&` or `const A&` where A is one of Ts
    using type = std::remove_reference_t<Op>;
    constexpr int arity = detail::get_arity(static_cast<type*>(nullptr));
    constexpr bool is_dynamic = detail::is_dynamic(static_cast<type*>(nullptr));

    if constexpr (is_dynamic) {
        return unpackDynamic(std::make_index_sequence<arity>(), op, cb);
    } else {
        return unpackFixed(std::make_index_sequence<arity>(), op, cb);
    }
}

/**
 * Helper for unpacking child-results from a vector.
 *
 * Calls the callback with:
 *   - the vector of dynamic child-results, if 'is_dynamic'.
 *   - the static child-results as separate arguments.
 */
template <bool is_dynamic, typename Result, size_t... I>
auto unpackResults(size_t dynamicArity,
                   std::index_sequence<I...>,
                   std::span<Result> stack,
                   auto&& cb) {
    constexpr size_t arity = sizeof...(I);
    // In left-to-right traversal order the dynamic children come first, so they
    // were pushed earlier. In both cases the static results are at the end.
    std::span<Result> staticResults = stack.last(arity);
    if constexpr (is_dynamic) {
        // The dynamic results are immediately before the static results.
        std::span<Result> dynResultSpan = stack.last(dynamicArity + arity).first(dynamicArity);
        std::vector<Result> dynResults;
        dynResults.reserve(dynamicArity);
        for (Result& r : dynResultSpan) {
            dynResults.push_back(std::move(r));
        }
        return cb(std::move(dynResults), std::move(staticResults[I])...);
    } else {
        return cb(std::move(staticResults[I])...);
    }
}

/**
 * Allows navigating / iterating a tree of PolyValue / Operator nodes.
 *
 * Holds references to the 'current()' node being visited and all of its ancestors.
 * Therefore, the caller must not mutate any of those ancestors. It's safe to mutate 'current()',
 * but mutating any node above the current would invalidate some of those references.
 *
 * 'Tree' is the value type of the root node that the caller has. It could be:
 *   - PolyValue
 *   - const PolyValue
 *   - PolyValue::reference_type
 *   - const PolyValue::reference_type
 *
 * 'Ref' is the type of references that are returned on each iteration,
 * and are stored on the stack internally. It could be:
 *   - PolyValue&
 *   - const PolyValue&
 *   - PolyValue::reference_type
 *   - const PolyValue::reference_type
 * It's important not to use a type like 'reference_type&', because the reference_type it points to
 * would necessarily be temporary, because an operator tree doesn't store reference_type.
 */
template <typename Tree, typename Ref>
requires(!std::is_reference_v<Tree>)  //
class TreeCursor {
public:
    // Represents one-past-the-end.
    TreeCursor() {}
    // Uses 'tree' as the root.
    TreeCursor(Tree& tree) : _stack{Entry{0, 1, tree}}, _entering(true) {}
    TreeCursor(typename Tree::reference_type ref) : _stack{Entry{0, 1, ref}}, _entering(true) {}

    // True if the cursor is one-past-the-end.
    bool done() const {
        return _stack.empty();
    }

    // True if we are visiting the current node for the first time, on the way down.
    // False if we are visiting it for the second and last time, on the way up.
    bool isEntering() const {
        return _entering;
    }

    // 'remove_const_t' here because shallow const does not affect the function signature, and
    // clang-tidy complains.
    std::remove_const_t<Ref> current() const {
        return _stack.back().node;
    }

    void advance() {
        if (_entering) {
            // Try entering first child; otherwise leave current node.
            auto& [index, parentArity, node] = _stack.back();
            node.visit([&](auto&&, auto& op) {
                int arity = getTotalArity(op);
                if (arity > 0) {
                    _stack.push_back(Entry{0, arity, getChild(op, 0)});
                } else {
                    _entering = false;
                }
            });
        } else {
            // Try entering next sibling; otherwise leave parent node.
            auto& [index, parentArity, node] = _stack.back();
            int next = index + 1;
            if (next < parentArity) {
                // NOTE visit() cannot return a reference, because it's declared 'auto visit(...)'.
                // Consider using decltype(auto) there, like std::visit does.
                // Work around it by putting everything inside the lambda.
                Entry& parent = _stack.rbegin()[1];
                parent.node.visit([&, parentArity = parentArity](auto&&, auto& op) {
                    _stack.pop_back();
                    _stack.push_back(Entry{next, parentArity, getChild(op, next)});
                    _entering = true;
                });
            } else {
                _stack.pop_back();
                // '_entering' is still false.
            }
        }
    }

private:
    template <typename Slot, int Arity>
    static int getTotalArity(const OpFixedArity<Slot, Arity>&) {
        return Arity;
    }

    template <typename Slot, int Arity>
    static int getTotalArity(const OpDynamicArity<Slot, Arity>& op) {
        return op.nodes().size() + Arity;
    }

    // Get the 'i'th child, where 'i' is in the range [0, get_total_arity).
    //
    // If there are any dynamic children, then they are ordered before static children, just like
    // the constructor of OpDynamicArity. So get_child(op, 0) is always the first dynamic child if
    // there is one.
    //
    // 'remove_const_t' here because shallow const does not affect the function signature, and
    // clang-tidy complains.
    template <typename Op>
    std::remove_const_t<Ref> getChild(Op& op, int i) {
        if constexpr (is_dynamic(static_cast<Op*>(nullptr))) {
            if constexpr (get_arity(static_cast<Op*>(nullptr)) > 0) {
                // Dynamic and static children.
                int dynSize = op.nodes().size();
                if (i >= dynSize) {
                    return op._nodes[i - dynSize];
                }
            }
            return op.nodes()[i];
        } else if constexpr (get_arity(static_cast<Op*>(nullptr)) > 0) {
            // Only static children.
            return op._nodes[i];
        } else {
            MONGO_UNREACHABLE_TASSERT(7835401);
        }
    }

    struct Entry {
        // 'index' is the position of 'node' within its parent.
        // If 'node' is the root, then 'index' is 0.
        int index;
        // 'parentArity' is the number of children the parent of 'node' has.
        // If 'node' is the root, then 'parentArity' is 1.
        // An Entry is only valid as long as 'index < parentArity'.
        int parentArity;
        // Points to an ancestor of the current() node, or the current node itself.
        Ref node;
    };
    // '_stack.back().node' is the current node and '_stack[0].node' is the root.
    // When iteration is done, we represent past-the-end by an empty _stack.
    std::vector<Entry> _stack;
    // '_entering=true' means we are visiting a node before we have visited its children.
    // '_entering=false' means we are "leaving" it: visiting it after its children.
    bool _entering;
};
template <typename... Ts>
TreeCursor(PolyValue<Ts...>&) -> TreeCursor<PolyValue<Ts...>, PolyValue<Ts...>&>;
template <typename... Ts>
TreeCursor(const PolyValue<Ts...>&) -> TreeCursor<const PolyValue<Ts...>, const PolyValue<Ts...>&>;

template <typename Ref>
TreeCursor(Ref&&) -> TreeCursor<typename Ref::polyvalue_type, Ref>;

template <typename Ref>
TreeCursor(const Ref&&) -> TreeCursor<const typename Ref::polyvalue_type, const Ref>;

template <typename Ref>
TreeCursor(Ref&) -> TreeCursor<typename Ref::polyvalue_type, Ref>;

template <typename Ref>
TreeCursor(const Ref&) -> TreeCursor<const typename Ref::polyvalue_type, const Ref>;

}  // namespace detail

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
 * before traversing its children. The traversal is guaranteed to use depth-first, left-to-right
 * order.
 *
 * This method also allows propagating results from the traversal implicitly via the return type of
 * the methods in D. For instance, to return an integer after traversal and a node which has two
 * children, the signature would look something like this:
 *
 *      int transport(const NodeType&, int childResult0, int childResult1)
 *
 * See 'algebra_test.cpp' for more examples.
 */
template <bool withSlot,
          typename D,
          typename N,
          typename... ExtraArgs,
          typename Result = detail::deduced_t<D, withSlot, N, ExtraArgs...>>
Result transport(N&& root, D& domain, ExtraArgs&&... extraArgs) {
    constexpr bool isVoid = std::is_same_v<Result, void>;
    std::conditional_t<isVoid,
                       // If it's a void transport we don't need the results stack.
                       std::nullptr_t,
                       // Use boost vector to avoid std::vector<bool> special case.
                       boost::container::vector<Result>>
        results;

    for (detail::TreeCursor cursor{root}; !cursor.done(); cursor.advance()) {
        auto&& node = cursor.current();
        if (cursor.isEntering()) {
            // Pass the domain through lambda arguments, because capturing it results in a compile
            // error (which each compiler may or may not detect: at time of writing clang and msvc
            // detect it and gcc does not).
            //
            // When a constexpr if evaluates to false, it "discards" the body, but "the discarded
            // statement cannot be ill-formed for every possible specialization". Presumably this
            // rule is intended to catch mistakes early, when the template is defined.
            //
            // If the 'domain' type has no prepare() methods, and we capture it, then
            // 'domain.prepare(/* ... */)' looks like a mistake according to this rule. Any
            // instantiation of the lambda makes this call ill-formed.
            //
            // Passing the 'd' as an argument means the validity of 'd.prepare()' depends on how
            // the lambda is instantiated, so we no longer violate this rule.
            node.visit(
                [&](auto&& slot, auto&& op, auto&& d /*domain*/) {
                    // Each overload of prepare() is optional, so check whether this
                    // particular overload is implemented.
                    if constexpr (detail::has_prepare_v<withSlot,
                                                        decltype(slot),
                                                        decltype(d),
                                                        std::remove_reference_t<decltype(op)>,
                                                        ExtraArgs...>) {
                        // prepare() does not take child-results because they haven't been
                        // computed yet. For consistency it doesn't take the children,
                        // either.
                        if constexpr (withSlot) {
                            d.prepare(std::forward<decltype(slot)>(slot),
                                      std::forward<decltype(op)>(op),
                                      extraArgs...);
                        } else {
                            d.prepare(std::forward<decltype(op)>(op), extraArgs...);
                        }
                    }
                },
                domain);

        } else {
            node.visit([&](auto&& slot, auto&& op) {
                using Op = std::remove_reference_t<decltype(op)>;
                constexpr int arity = detail::get_arity(static_cast<Op*>(nullptr));
                constexpr bool is_dynamic = detail::is_dynamic(static_cast<Op*>(nullptr));

                if constexpr (isVoid) {
                    // Pass the node's children, instead of child results.
                    detail::unpack(op, [&](auto&&... children) {
                        if constexpr (withSlot) {
                            domain.transport(std::forward<decltype(slot)>(slot),
                                             std::forward<decltype(op)>(op),
                                             extraArgs...,
                                             std::forward<decltype(children)>(children)...);
                        } else {
                            domain.transport(std::forward<decltype(op)>(op),
                                             extraArgs...,
                                             std::forward<decltype(children)>(children)...);
                        }
                    });
                } else {
                    size_t dynamicArity = 0;
                    if constexpr (is_dynamic) {
                        dynamicArity = op.nodes().size();
                    }
                    size_t totalArity = dynamicArity + arity;

                    // Unpack child results, and call transport() to get a new combined result.
                    Result r = detail::unpackResults<is_dynamic>(
                        dynamicArity,
                        std::make_index_sequence<arity>(),
                        std::span{results.data(), results.size()},
                        [&](auto&&... childResults) {
                            if constexpr (withSlot) {
                                return domain.transport(
                                    std::forward<decltype(slot)>(slot),
                                    std::forward<decltype(op)>(op),
                                    extraArgs...,
                                    std::forward<decltype(childResults)>(childResults)...);
                            } else {
                                return domain.transport(
                                    std::forward<decltype(op)>(op),
                                    extraArgs...,
                                    std::forward<decltype(childResults)>(childResults)...);
                            }
                        });

                    // Replace child results with combined result.
                    for (size_t i = 0; i < totalArity; ++i) {
                        results.pop_back();
                    }
                    results.push_back(std::move(r));
                }
            });
        }
    }

    if constexpr (!isVoid) {
        return std::move(results.front());
    }
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

}  // namespace mongo::algebra
