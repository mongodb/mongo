/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

/**
 * This header describes a mechanism for making "decorable" types.
 *
 * A decorable type is one to which various subsystems may attach subsystem-private data, so long as
 * they declare what that data will be before any instances of the decorable type are created.
 *
 * For example, suppose you had a class Client, representing on a server a network connection to a
 * client process.  Suppose that your server has an authentication module, that attaches data to the
 * client about authentication.  If class Client looks something like this:
 *
 * class Client : public Decorable<Client>{
 * ...
 * };
 *
 * Then the authentication module, before the first client object is created, calls
 *
 *     const auto authDataDescriptor = Client::declareDecoration<AuthenticationPrivateData>();
 *
 * And stores authDataDescriptor in a module-global variable,
 *
 * And later, when it has a Client object, client, and wants to get at the per-client
 * AuthenticationPrivateData object, it calls
 *
 *    authDataDescriptor(client)
 *
 * to get a reference to the AuthenticationPrivateData for that client object.
 *
 * With this approach, individual subsystems get to privately augment the client object via
 * declarations local to the subsystem, rather than in the global client header.
 */

#pragma once

#include <algorithm>
#include <boost/optional.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

namespace decorable_detail {

template <typename T>
constexpr inline bool pretendTrivialInit = false;
template <typename T>
constexpr inline bool pretendTrivialInit<std::unique_ptr<T>> = true;
template <typename T>
constexpr inline bool pretendTrivialInit<std::shared_ptr<T>> = true;
template <typename T>
constexpr inline bool pretendTrivialInit<boost::optional<T>> = true;

struct LifecycleOperations {
    using CtorFn = void(void*);
    using DtorFn = void(void*);
    using CopyFn = void(void*, const void*);
    using AssignFn = void(void*, const void*);

    template <typename T>
    static constexpr CtorFn* getCtor() {
        if constexpr (!std::is_trivially_constructible_v<T> && !pretendTrivialInit<T>)
            return +[](void* p) {
                new (p) T;
            };
        return nullptr;
    }

    template <typename T>
    static constexpr DtorFn* getDtor() {
        if constexpr (!std::is_trivially_destructible_v<T>)
            return +[](void* p) {
                static_cast<T*>(p)->~T();
            };
        return nullptr;
    }

    template <typename T, bool needCopy>
    static constexpr CopyFn* getCopy() {
        if constexpr (needCopy)
            return +[](void* p, const void* src) {
                new (p) T(*static_cast<const T*>(src));
            };
        return nullptr;
    }

    template <typename T, bool needCopy>
    static constexpr AssignFn* getAssign() {
        if constexpr (needCopy)
            return +[](void* p, const void* src) {
                *static_cast<T*>(p) = *static_cast<const T*>(src);
            };
        return nullptr;
    }

    template <typename T, bool needCopy>
    static constexpr LifecycleOperations make() {
        return {getCtor<T>(), getDtor<T>(), getCopy<T, needCopy>(), getAssign<T, needCopy>()};
    };

    CtorFn* ctor;     /** null if trivial */
    DtorFn* dtor;     /** null if trivial */
    CopyFn* copy;     /** null if no copy */
    AssignFn* assign; /** null if no assignment */
};

template <typename T, bool needCopy>
constexpr inline LifecycleOperations lifecycleOperations = LifecycleOperations::make<T, needCopy>();

class Registry {
public:
    struct Entry {
        const std::type_info* typeInfo;
        ptrdiff_t offset;
        const LifecycleOperations* ops;
        size_t size;
        size_t align;
    };

    /** Return registry position of new entry. */
    template <typename T>
    size_t declare(const LifecycleOperations* ops) {
        static constexpr auto al = alignof(T);
        static constexpr auto sz = sizeof(T);
        ptrdiff_t offset = (_bufferSize + al - 1) / al * al;  // pad to alignment
        _entries.push_back({&typeid(T), offset, ops, sz, al});
        _bufferSize = offset + sz;
        _bufferAlignment = std::max(_bufferAlignment, al);
        return _entries.size() - 1;
    }

    size_t bufferSize() const {
        return _bufferSize;
    }

    size_t bufferAlignment() const {
        return _bufferAlignment;
    }

    auto begin() const {
        return _entries.begin();
    }
    auto end() const {
        return _entries.end();
    }
    size_t size() const {
        return _entries.size();
    }
    const auto& operator[](size_t i) const {
        invariant(i < size(), format(FMT_STRING("{} < {}"), i, size()));
        return _entries[i];
    }

private:
    std::vector<Entry> _entries;
    size_t _bufferSize = sizeof(void*);  // The owner pointer is always present.
    size_t _bufferAlignment = alignof(void*);
};

/** Defined for gdb pretty-printer visibility only. */
template <typename D>
inline const Registry* gdbRegistry = nullptr;

template <typename D>
Registry& getRegistry() {
    static auto reg = [] {
        auto r = new Registry{};
        gdbRegistry<D> = r;
        return r;
    }();
    return *reg;
}

/**
 * A token that represents the 2-way link between Decorable D and a decoration
 * of type T attached to it. Given a Decorable, identifies a Decoration. Going
 * in the other direction, it can also be used to identify a decoration's owner.
 */
template <typename D, typename T>
class DecorationToken {
public:
    using DecoratedType = D;
    using DecorationType = T;

    explicit DecorationToken(size_t registryPosition) : _registryPosition{registryPosition} {}

    /**
     * Returns a reference to this decoration object in the specified `Decorable d`.
     * Const and non-const overloads are provided for deep const semantics.
     */
    const DecorationType& decoration(const DecoratedType& d) const {
        return d.decoration(*this);
    }
    DecorationType& decoration(DecoratedType& d) const {
        return d.decoration(*this);
    }

    /**
     * Returns the owner of decoration `t`, with deep const semantics.
     * Decoration `t` can also be given as a pointer, in which case a pointer to
     * its owner is returned.
     */
    const DecoratedType& owner(const DecorationType& t) const {
        // The decoration block starts with a backlink to the decorable.
        const void* p = &t;
        const void* block = static_cast<const char*>(p) - _offset;
        return *DecoratedType::downcastBackLink(*reinterpret_cast<const void* const*>(block));
    }
    DecoratedType& owner(DecorationType& t) const {
        return const_cast<DecoratedType&>(owner(std::as_const(t)));
    }
    const DecoratedType* owner(const DecorationType* t) const {
        return &owner(*t);
    }
    DecoratedType* owner(DecorationType* t) const {
        return &owner(*t);
    }

    /**
     * Syntactic sugar, equivalent to decoration(d). As a convenience, overloads
     * are provided so that a pointer `&d` can be given instead.
     */
    const DecorationType& operator()(const DecoratedType& d) const {
        return decoration(d);
    }
    DecorationType& operator()(DecoratedType& d) const {
        return decoration(d);
    }
    const DecorationType& operator()(const DecoratedType* d) const {
        return decoration(*d);
    }
    DecorationType& operator()(DecoratedType* d) const {
        return decoration(*d);
    }

    size_t registryPosition() const {
        return _registryPosition;
    }

    ptrdiff_t offsetInBlock() const {
        return _offset;
    }

private:
    size_t _registryPosition;
    ptrdiff_t _offset = decorable_detail::getRegistry<DecoratedType>()[_registryPosition].offset;
};

template <typename D>
class DecorationBuffer {
public:
    using DecoratedType = D;

    template <typename T>
    static DecorationToken<DecoratedType, T> declareDecoration() {
        // If DecoratedType has either of the copy operations, then T needs them both.
        static constexpr bool needCopy =
            std::is_copy_constructible_v<DecoratedType> || std::is_copy_assignable_v<DecoratedType>;
        return DecorationToken<DecoratedType, T>{
            _reg().template declare<T>(&lifecycleOperations<T, needCopy>)};
    }

    template <typename DecoratedBase>
    explicit DecorationBuffer(DecoratedBase* decorated) {
        _setBackLink(decorated);
        _constructorCommon();
    }

    /** Used when copying, but we need the decorated's address. */
    template <typename DecoratedBase>
    DecorationBuffer(DecoratedBase* decorated, const DecorationBuffer& other) {
        _setBackLink(decorated);
        const auto& reg = _reg();
        size_t n = reg.size();
        size_t i = 0;
        try {
            for (; i != n; ++i) {
                const auto& e = reg[i];
                e.ops->copy(getAtOffset(e.offset), other.getAtOffset(e.offset));
            }
        } catch (...) {
            _tearDownParts(i);
            throw;
        }
    }

    ~DecorationBuffer() {
        _tearDownParts(_reg().size());
    }

    /** Only basic (not strong) exception safety for this copy-assign. */
    DecorationBuffer& operator=(const DecorationBuffer& other) {
        if (this == &other) {
            return *this;
        }
        auto& reg = _reg();
        size_t n = reg.size();
        for (size_t i = 0; i != n; ++i) {
            const auto& e = reg[i];
            e.ops->assign(getAtOffset(e.offset), other.getAtOffset(e.offset));
        }
        return *this;
    }

    const void* getAtOffset(ptrdiff_t offset) const {
        return _data + offset;
    }
    void* getAtOffset(ptrdiff_t offset) {
        return const_cast<void*>(std::as_const(*this).getAtOffset(offset));
    }

private:
    struct AlignedDeleter {
        void operator()(unsigned char* ptr) const {
            ::operator delete(ptr, size, alignment);
        }

        std::size_t size;
        std::align_val_t alignment;
    };

    using OwningPointer = std::unique_ptr<unsigned char, AlignedDeleter>;

    static Registry& _reg() {
        return getRegistry<DecoratedType>();
    }

    void _constructorCommon() {
        auto& reg = _reg();
        size_t n = reg.size();
        size_t i = 0;
        try {
            for (; i != n; ++i) {
                const auto& e = reg[i];
                if (const auto& ctor = e.ops->ctor)
                    ctor(getAtOffset(e.offset));
            }
        } catch (...) {
            _tearDownParts(i);
            throw;
        }
    }

    void _tearDownParts(size_t count) noexcept {
        auto& reg = _reg();
        while (count--) {
            const auto& e = reg[count];
            if (const auto& dtor = e.ops->dtor)
                dtor(getAtOffset(e.offset));
        }
    }

    template <typename DecoratedBase>
    void _setBackLink(const DecoratedBase* decorated) {
        static_assert(std::is_base_of_v<DecoratedBase, DecoratedType>);
        *reinterpret_cast<const void**>(_data) = DecoratedType::upcastBackLink(decorated);
    }

    OwningPointer _makeData() {
        auto& reg = _reg();
        auto alignment = reg.bufferAlignment();
        auto sz = reg.bufferSize();
        auto rawBuffer =
            static_cast<unsigned char*>(::operator new(sz, std::align_val_t(alignment)));
        std::memset(rawBuffer, 0, sz);
        return OwningPointer{rawBuffer, AlignedDeleter{sz, std::align_val_t{alignment}}};
    }

    OwningPointer _dataOwnership{_makeData()};
    unsigned char* _data{_dataOwnership.get()};
};

}  // namespace decorable_detail

template <typename D>
class Decorable {
public:
    using DerivedType = D;  // CRTP

    template <typename T>
    using Decoration = decorable_detail::DecorationToken<DerivedType, T>;

    static const DerivedType* downcastBackLink(const void* vp) {
        return static_cast<const DerivedType*>(static_cast<const Decorable*>(vp));
    }

    static const void* upcastBackLink(const Decorable* deco) {
        return deco;
    }

    template <typename T>
    static Decoration<T> declareDecoration() {
        static_assert(std::is_nothrow_destructible_v<T>);
        return decorable_detail::DecorationBuffer<DerivedType>::template declareDecoration<T>();
    }

    Decorable() : _decorations{this} {}

    Decorable(const Decorable& o) : _decorations{this, o._decorations} {}

    virtual ~Decorable() = default;

    const auto& decorations() const {
        return _decorations;
    }
    auto& decorations() {
        return _decorations;
    }

    template <typename T>
    const T& decoration(const Decoration<T>& deco) const {
        return *static_cast<const T*>(_decorations.getAtOffset(deco.offsetInBlock()));
    }
    template <typename T>
    T& decoration(const Decoration<T>& deco) {
        return const_cast<T&>(std::as_const(*this).decoration(deco));
    }

    /**
     * A `Decoration<T>` can be used as a maplike key.
     * So `x[deco]` is syntactic sugar for `x.decoration(deco)`.
     * Ex:
     *    class X : Decorable<X> { ... };
     *    auto deco = X::declareDecoration<int>();
     *    ...
     *        X x;
     *        X* xp = &x;
     *
     *        x[deco] = 123;             // map-like syntax
     *        x.decoration(deco) = 123;  // equivalent
     *        deco(x) = 123;             // older deco as callable syntax
     *
     *        // pointer syntax
     *        (*xp)[deco] = 123;           // map-like syntax
     *        xp->decoration(deco) = 123;  // equivalent
     *        deco(xp) = 123;              // older deco as callable syntax
     */
    template <typename T>
    const T& operator[](const Decoration<T>& deco) const {
        return decoration(deco);
    }
    template <typename T>
    T& operator[](const Decoration<T>& deco) {
        return decoration(deco);
    }

private:
    decorable_detail::DecorationBuffer<DerivedType> _decorations;
};

}  // namespace mongo
