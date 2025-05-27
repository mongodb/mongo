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

#include "mongo/util/assert_util.h"

#include <array>
#include <stdexcept>
#include <type_traits>

namespace mongo::algebra {
namespace detail {

template <typename T, typename... Args>
inline constexpr bool is_one_of_v = std::disjunction_v<std::is_same<T, Args>...>;

template <typename... Args>
struct is_unique_t : std::true_type {};

template <typename H, typename... T>
struct is_unique_t<H, T...>
    : std::bool_constant<!is_one_of_v<H, T...> && is_unique_t<T...>::value> {};

template <typename... Args>
inline constexpr bool is_unique_v = is_unique_t<Args...>::value;

/**
 * Given the type T find its index in Ts.
 */
template <typename T, typename... Ts>
static inline constexpr int find_index() {
    static_assert(is_unique_v<Ts...>, "Types must be unique");
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

/**
 * The base control block that PolyValue holds.
 *
 * It does not contain anything else except for the runtime tag.
 */
template <typename... Ts>
class ControlBlock {
    const int _tag;

protected:
    ControlBlock(int tag) : _tag(tag) {}

public:
    auto getRuntimeTag() const {
        return _tag;
    }
};

/**
 * The concrete control block VTable generator.
 *
 * It must be empty as PolyValue derives from the generators and we want EBO to kick in.
 */
template <typename T, typename... Ts>
class ControlBlockVTable {
protected:
    static constexpr int _staticTag = detail::find_index<T, Ts...>();
    static_assert(_staticTag != -1, "Type must be on the list");

    using AbstractType = ControlBlock<Ts...>;

    /**
     * The concrete control block for every type T of Ts. Derives from a ControlBlock which holds
     * the runtime type tag for T.
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

    static constexpr auto concrete(AbstractType* block) {
        return static_cast<ConcreteType*>(block);
    }

    static constexpr auto concrete(const AbstractType* block) {
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

    static void destroy(AbstractType* block) {
        delete concrete(block);
    }

    static bool compareEq(AbstractType* blockLhs, AbstractType* blockRhs) {
        tassert(9895600,
                "To compare the types they need to be non-null",
                (blockLhs != nullptr && blockRhs != nullptr));
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

    template <typename Callback, typename N, typename... Args>
    static auto visit(Callback&& cb, N& holder, AbstractType* block, Args&&... args) {
        return cb(holder, *cast<T>(block), std::forward<Args>(args)...);
    }

    template <typename Callback, typename N, typename... Args>
    static auto visitConst(Callback&& cb,
                           const N& holder,
                           const AbstractType* block,
                           Args&&... args) {
        return cb(holder, *castConst<T>(block), std::forward<Args>(args)...);
    }
};

/**
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
public:
    using key_type = int;

private:
    static_assert(detail::is_unique_v<Ts...>, "Types must be unique");
    static_assert(std::conjunction_v<std::is_empty<ControlBlockVTable<Ts, Ts...>>...>,
                  "VTable base classes must be empty");

    // Static array that allows lookup into methods on ControlBlockVTable using the PolyValue tag.
    static constexpr std::array cloneTbl = {&ControlBlockVTable<Ts, Ts...>::clone...};

    ControlBlock<Ts...>* _object{nullptr};

    PolyValue(ControlBlock<Ts...>* object) : _object(object) {}

    auto tag() const {
        return _object->getRuntimeTag();
    }

    static void check(const ControlBlock<Ts...>* object) {
        tassert(6232700, "PolyValue is empty", object != nullptr);
    }

    static void destroy(ControlBlock<Ts...>* object) {
        static constexpr std::array destroyTbl = {&ControlBlockVTable<Ts, Ts...>::destroy...};

        destroyTbl[object->getRuntimeTag()](object);
    }

    template <typename T>
    static T* cast(ControlBlock<Ts...>* object) {
        check(object);
        static constexpr std::array castTbl = {&ControlBlockVTable<Ts, Ts...>::template cast<T>...};
        return castTbl[object->getRuntimeTag()](object);
    }

    template <typename T>
    static const T* castConst(ControlBlock<Ts...>* object) {
        check(object);
        static constexpr std::array castTbl = {
            &ControlBlockVTable<Ts, Ts...>::template castConst<T>...};
        return castTbl[object->getRuntimeTag()](object);
    }

    template <typename T>
    static bool is(ControlBlock<Ts...>* object) {
        check(object);
        static constexpr std::array isTbl = {ControlBlockVTable<Ts, Ts...>::template is_v<T>...};
        return isTbl[object->getRuntimeTag()];
    }

    class CompareHelper {
        ControlBlock<Ts...>* _object{nullptr};

        auto tag() const {
            tassert(9895601, "PolyValue is empty", _object != nullptr);
            return _object->getRuntimeTag();
        }

    public:
        CompareHelper() = default;
        CompareHelper(ControlBlock<Ts...>* object) : _object(object) {}

        bool operator==(const CompareHelper& rhs) const {
            static constexpr std::array cmp = {ControlBlockVTable<Ts, Ts...>::compareEq...};
            return cmp[tag()](_object, rhs._object);
        }
    };

    class Reference {
        ControlBlock<Ts...>* _object{nullptr};


        auto tag() const {
            tassert(9895602, "PolyValue is empty", _object != nullptr);
            return _object->getRuntimeTag();
        }

    public:
        Reference() = default;
        Reference(ControlBlock<Ts...>* object) : _object(object) {}
        using polyvalue_type = PolyValue;

        // Reference is implicitly convertible from PolyValue. This conversion is equivalent to the
        // caller using .ref() explicitly. Having this conversion makes it easier to call functions
        // that take a Reference, which encourages functions that minimize their dependencies, by
        // taking Reference instead of 'const PolyValue&' where possible.
        Reference(const PolyValue& n) : Reference(n.ref()) {}

        template <int I>
        using get_t = detail::get_type_by_index<I, Ts...>;

        key_type tagOf() const {
            check(_object);

            return tag();
        }

        template <typename Callback, typename... Args>
        auto visit(Callback&& cb, Args&&... args) {
            // unfortunately gcc rejects much nicer code, clang and msvc accept
            // static constexpr std::array visitTbl = { &ControlBlockVTable<Ts, Ts...>::template
            // visit<V>... };

            using FunPtrType =
                decltype(&ControlBlockVTable<get_t<0>,
                                             Ts...>::template visit<Callback, Reference, Args...>);
            static constexpr FunPtrType visitTbl[] = {
                &ControlBlockVTable<Ts, Ts...>::template visit<Callback, Reference, Args...>...};

            check(_object);
            return visitTbl[tag()](
                std::forward<Callback>(cb), *this, _object, std::forward<Args>(args)...);
        }

        template <typename Callback, typename... Args>
        auto visit(Callback&& cb, Args&&... args) const {
            // unfortunately gcc rejects much nicer code, clang and msvc accept
            // static constexpr std::array visitTbl = { &ControlBlockVTable<Ts, Ts...>::template
            // visitConst<V>... };

            using FunPtrType = decltype(&ControlBlockVTable<get_t<0>, Ts...>::
                                            template visitConst<Callback, Reference, Args...>);
            static constexpr FunPtrType visitTbl[] = {
                &ControlBlockVTable<Ts,
                                    Ts...>::template visitConst<Callback, Reference, Args...>...};

            check(_object);
            return visitTbl[tag()](
                std::forward<Callback>(cb), *this, _object, std::forward<Args>(args)...);
        }

        template <typename T>
        T* cast() {
            return PolyValue<Ts...>::template cast<T>(_object);
        }

        template <typename T>
        const T* cast() const {
            return PolyValue<Ts...>::template castConst<T>(_object);
        }

        template <typename T>
        bool is() const {
            return PolyValue<Ts...>::template is<T>(_object);
        }

        bool empty() const {
            return !_object;
        }

        void swap(Reference& other) noexcept {
            std::swap(other._object, _object);
        }

        // Compare references, not the objects themselves.
        bool operator==(const Reference& rhs) const {
            return _object == rhs._object;
        }

        bool operator==(const PolyValue& rhs) const {
            return rhs == (*this);
        }

        auto hash() const noexcept {
            return std::hash<const void*>{}(_object);
        }

        auto follow() const {
            return CompareHelper(_object);
        }

        // PolyValue is constructible from Reference, but only explicitly.
        // This .copy() helper may be clearer than an explicit constructor call.
        PolyValue copy() const {
            return PolyValue{*this};
        }

        friend class PolyValue;
    };

public:
    using reference_type = Reference;

    template <typename T>
    static constexpr key_type tagOf() {
        return ControlBlockVTable<T, Ts...>::_staticTag;
    }

    key_type tagOf() const {
        check(_object);
        return tag();
    }

    PolyValue() = delete;

    PolyValue(const PolyValue& other) {
        if (other._object) {
            _object = cloneTbl[other.tag()](other._object);
        }
    }

    explicit PolyValue(const Reference& other) {
        if (other._object) {
            _object = cloneTbl[other.tag()](other._object);
        }
    }

    PolyValue(PolyValue&& other) noexcept {
        swap(other);
    }

    ~PolyValue() {
        if (_object) {
            destroy(_object);
        }
    }

    PolyValue& operator=(PolyValue other) {
        swap(other);
        return *this;
    }

    template <typename T, typename... Args>
    static PolyValue make(Args&&... args) {
        return PolyValue{ControlBlockVTable<T, Ts...>::make(std::forward<Args>(args)...)};
    }

    template <int I>
    using get_t = detail::get_type_by_index<I, Ts...>;

    template <typename Callback, typename... Args>
    auto visit(Callback&& cb, Args&&... args) {
        // unfortunately gcc rejects much nicer code, clang and msvc accept
        // static constexpr std::array visitTbl = { &ControlBlockVTable<Ts, Ts...>::template
        // visit<V>... };

        using FunPtrType =
            decltype(&ControlBlockVTable<get_t<0>,
                                         Ts...>::template visit<Callback, PolyValue, Args...>);
        static constexpr FunPtrType visitTbl[] = {
            &ControlBlockVTable<Ts, Ts...>::template visit<Callback, PolyValue, Args...>...};

        check(_object);
        return visitTbl[tag()](
            std::forward<Callback>(cb), *this, _object, std::forward<Args>(args)...);
    }

    template <typename Callback, typename... Args>
    auto visit(Callback&& cb, Args&&... args) const {
        // unfortunately gcc rejects much nicer code, clang and msvc accept
        // static constexpr std::array visitTbl = { &ControlBlockVTable<Ts, Ts...>::template
        // visitConst<V>... };

        using FunPtrType =
            decltype(&ControlBlockVTable<get_t<0>,
                                         Ts...>::template visitConst<Callback, PolyValue, Args...>);
        static constexpr FunPtrType visitTbl[] = {
            &ControlBlockVTable<Ts, Ts...>::template visitConst<Callback, PolyValue, Args...>...};

        check(_object);
        return visitTbl[tag()](
            std::forward<Callback>(cb), *this, _object, std::forward<Args>(args)...);
    }

    template <typename T>
    T* cast() {
        return cast<T>(_object);
    }

    template <typename T>
    const T* cast() const {
        return castConst<T>(_object);
    }

    template <typename T>
    bool is() const {
        return is<T>(_object);
    }

    bool empty() const {
        return !_object;
    }

    void swap(PolyValue& other) noexcept {
        std::swap(other._object, _object);
    }

    bool operator==(const PolyValue& rhs) const {
        static constexpr std::array cmpTbl = {ControlBlockVTable<Ts, Ts...>::compareEq...};
        return cmpTbl[tag()](_object, rhs._object);
    }

    bool operator==(const Reference& rhs) const {
        static constexpr std::array cmpTbl = {ControlBlockVTable<Ts, Ts...>::compareEq...};
        return cmpTbl[tag()](_object, rhs._object);
    }

    auto ref() {
        check(_object);
        return Reference(_object);
    }

    auto ref() const {
        check(_object);
        return Reference(_object);
    }
};

}  // namespace mongo::algebra
