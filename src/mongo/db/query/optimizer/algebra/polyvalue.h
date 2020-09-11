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

#include <array>
#include <stdexcept>
#include <type_traits>

namespace mongo::optimizer {
namespace algebra {
namespace detail {

template <typename T, typename... Args>
inline constexpr bool is_one_of_v = std::disjunction_v<std::is_same<T, Args>...>;

template <typename T, typename... Args>
inline constexpr bool is_one_of_f() {
    return is_one_of_v<T, Args...>;
}

template <typename... Args>
struct is_unique_t : std::true_type {};

template <typename H, typename... T>
struct is_unique_t<H, T...>
    : std::bool_constant<!is_one_of_f<H, T...>() && is_unique_t<T...>::value> {};

template <typename... Args>
inline constexpr bool is_unique_v = is_unique_t<Args...>::value;

// Given the type T find its index in Ts
template <typename T, typename... Ts>
static inline constexpr int find_index() {
    static_assert(detail::is_unique_v<Ts...>, "Types must be unique");
    constexpr bool matchVector[] = {std::is_same<T, Ts>::value...};

    for (int index = 0; index < static_cast<int>(sizeof...(Ts)); ++index) {
        if (matchVector[index]) {
            return index;
        }
    }

    return -1;
}

template <int N, typename T, typename... Ts>
struct get_type_by_index_impl {
    using type = typename get_type_by_index_impl<N - 1, Ts...>::type;
};
template <typename T, typename... Ts>
struct get_type_by_index_impl<0, T, Ts...> {
    using type = T;
};

// Given the index I return the type from Ts
template <int I, typename... Ts>
using get_type_by_index = typename get_type_by_index_impl<I, Ts...>::type;

}  // namespace detail

/*=====-----
 *
 * The overload trick to construct visitors from lambdas.
 *
 */
template <class... Ts>
struct overload : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overload(Ts...)->overload<Ts...>;

/*=====-----
 *
 * Forward declarations
 *
 */
template <typename... Ts>
class PolyValue;

template <typename T, typename... Ts>
class ControlBlockVTable;

/*=====-----
 *
 * The base control block that PolyValue holds.
 *
 * It does not contain anything else by the runtime tag.
 *
 */
template <typename... Ts>
class ControlBlock {
    const int _tag;

protected:
    ControlBlock(int tag) noexcept : _tag(tag) {}

public:
    auto getRuntimeTag() const noexcept {
        return _tag;
    }
};

/*=====-----
 *
 * The concrete control block VTable generator.
 *
 * It must be empty ad PolyValue derives from the generators
 * and we want EBO to kick in.
 *
 */
template <typename T, typename... Ts>
class ControlBlockVTable {
    static constexpr int _staticTag = detail::find_index<T, Ts...>();
    static_assert(_staticTag != -1, "Type must be on the list");

    using AbstractType = ControlBlock<Ts...>;
    using PolyValueType = PolyValue<Ts...>;

    /*=====-----
     *
     * The concrete control block for every type T of Ts.
     *
     * It derives from the ControlBlock. All methods are private and only
     * the friend class ControlBlockVTable can call them.
     *
     */
    class ConcreteType : public AbstractType {
        T _t;

    public:
        template <typename... Args>
        ConcreteType(Args&&... args) : AbstractType(_staticTag), _t(std::forward<Args>(args)...) {}

        const T* getPtr() const {
            return &_t;
        }

        T* getPtr() {
            return &_t;
        }
    };

    static constexpr auto concrete(AbstractType* block) noexcept {
        return static_cast<ConcreteType*>(block);
    }

    static constexpr auto concrete(const AbstractType* block) noexcept {
        return static_cast<const ConcreteType*>(block);
    }

public:
    template <typename... Args>
    static AbstractType* make(Args&&... args) {
        return new ConcreteType(std::forward<Args>(args)...);
    }

    static AbstractType* clone(const AbstractType* block) {
        return new ConcreteType(*concrete(block));
    }

    static void destroy(AbstractType* block) noexcept {
        delete concrete(block);
    }

    static bool compareEq(AbstractType* blockLhs, AbstractType* blockRhs) noexcept {
        if (blockLhs->getRuntimeTag() == blockRhs->getRuntimeTag()) {
            return *castConst<T>(blockLhs) == *castConst<T>(blockRhs);
        }
        return false;
    }

    template <typename U>
    static constexpr bool is_v = std::is_base_of_v<U, T>;

    template <typename U>
    static U* cast(AbstractType* block) {
        if constexpr (is_v<U>) {
            return static_cast<U*>(concrete(block)->getPtr());
        } else {
            // gcc bug 81676
            (void)block;
            return nullptr;
        }
    }

    template <typename U>
    static const U* castConst(const AbstractType* block) {
        if constexpr (is_v<U>) {
            return static_cast<const U*>(concrete(block)->getPtr());
        } else {
            // gcc bug 81676
            (void)block;
            return nullptr;
        }
    }

    template <typename V, typename... Args>
    static auto visit(V&& v, PolyValueType& holder, AbstractType* block, Args&&... args) {
        return v(holder, *cast<T>(block), std::forward<Args>(args)...);
    }

    template <typename V, typename... Args>
    static auto visitConst(V&& v,
                           const PolyValueType& holder,
                           const AbstractType* block,
                           Args&&... args) {
        return v(holder, *castConst<T>(block), std::forward<Args>(args)...);
    }
};

/*=====-----
 *
 * This is a variation on variant and polymorphic value theme.
 *
 * A tag based dispatch
 *
 * Supported operations:
 * - construction
 * - destruction
 * - clone a = b;
 * - cast a.cast<T>()
 * - multi-method cast to common base a.cast<B>()
 * - multi-method visit
 */
template <typename... Ts>
class PolyValue : private ControlBlockVTable<Ts, Ts...>... {
    static_assert(detail::is_unique_v<Ts...>, "Types must be unique");
    static_assert(std::conjunction_v<std::is_empty<ControlBlockVTable<Ts, Ts...>>...>,
                  "VTable base classes must be empty");

    ControlBlock<Ts...>* _object{nullptr};

    PolyValue(ControlBlock<Ts...>* object) noexcept : _object(object) {}

    auto tag() const noexcept {
        return _object->getRuntimeTag();
    }

    void check() const {
        if (!_object) {
            throw std::logic_error("PolyValue is empty");
        }
    }

    static void destroy(ControlBlock<Ts...>* object) {
        static constexpr std::array destroyTbl = {&ControlBlockVTable<Ts, Ts...>::destroy...};

        destroyTbl[object->getRuntimeTag()](object);
    }

public:
    PolyValue() = delete;

    PolyValue(const PolyValue& other) {
        static constexpr std::array cloneTbl = {&ControlBlockVTable<Ts, Ts...>::clone...};
        if (other._object) {
            _object = cloneTbl[other.tag()](other._object);
        }
    }

    PolyValue(PolyValue&& other) noexcept {
        swap(other);
    }

    ~PolyValue() noexcept {
        if (_object) {
            destroy(_object);
        }
    }

    PolyValue& operator=(PolyValue other) noexcept {
        swap(other);
        return *this;
    }

    template <typename T, typename... Args>
    static PolyValue make(Args&&... args) {
        return PolyValue{ControlBlockVTable<T, Ts...>::make(std::forward<Args>(args)...)};
    }

    template <int I>
    using get_t = detail::get_type_by_index<I, Ts...>;

    template <typename V, typename... Args>
    auto visit(V&& v, Args&&... args) {
        // unfortunately gcc rejects much nicer code, clang and msvc accept
        // static constexpr std::array visitTbl = { &ControlBlockVTable<Ts, Ts...>::template
        // visit<V>... };

        using FunPtrType =
            decltype(&ControlBlockVTable<get_t<0>, Ts...>::template visit<V, Args...>);
        static constexpr FunPtrType visitTbl[] = {
            &ControlBlockVTable<Ts, Ts...>::template visit<V, Args...>...};

        check();
        return visitTbl[tag()](std::forward<V>(v), *this, _object, std::forward<Args>(args)...);
    }

    template <typename V, typename... Args>
    auto visit(V&& v, Args&&... args) const {
        // unfortunately gcc rejects much nicer code, clang and msvc accept
        // static constexpr std::array visitTbl = { &ControlBlockVTable<Ts, Ts...>::template
        // visitConst<V>... };

        using FunPtrType =
            decltype(&ControlBlockVTable<get_t<0>, Ts...>::template visitConst<V, Args...>);
        static constexpr FunPtrType visitTbl[] = {
            &ControlBlockVTable<Ts, Ts...>::template visitConst<V, Args...>...};

        check();
        return visitTbl[tag()](std::forward<V>(v), *this, _object, std::forward<Args>(args)...);
    }

    template <typename T>
    T* cast() {
        check();
        static constexpr std::array castTbl = {&ControlBlockVTable<Ts, Ts...>::template cast<T>...};
        return castTbl[tag()](_object);
    }

    template <typename T>
    const T* cast() const {
        static constexpr std::array castTbl = {
            &ControlBlockVTable<Ts, Ts...>::template castConst<T>...};

        check();
        return castTbl[tag()](_object);
    }

    template <typename T>
    bool is() const {
        static constexpr std::array isTbl = {ControlBlockVTable<Ts, Ts...>::template is_v<T>...};

        check();
        return isTbl[tag()];
    }

    bool empty() const {
        return !_object;
    }

    void swap(PolyValue& other) noexcept {
        std::swap(other._object, _object);
    }

    bool operator==(const PolyValue& rhs) const noexcept {
        static constexpr std::array cmp = {ControlBlockVTable<Ts, Ts...>::compareEq...};
        return cmp[tag()](_object, rhs._object);
    }
};

}  // namespace algebra
}  // namespace mongo::optimizer
