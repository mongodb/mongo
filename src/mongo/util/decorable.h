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
#include <atomic>  // NOLINT
#include <boost/optional.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include "mongo/platform/pause.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

class BSONObj;

namespace decorable_detail {

/**
 * The allowLazy trait should be specialized to true for types known to have
 * no side-effects in their nullary value-initialization. This knowledge allows
 * some decorations to be initialized only on first use.
 *
 * About half of the decorations we use can be detected as allowLazy=true types.
 * Anything that can be trivially initialized by a zero representation should
 * not be lazy. They use a a different and faster optimization.
 */

template <typename T, typename = void>
constexpr inline bool allowLazy = false;

// Types with the 'is_lazy_decoration` typedef are lazy by default.
template <typename T>
using HasIsLazyDecorationOp = typename T::is_lazy_decoration;
template <typename T>
constexpr inline bool
    allowLazy<T, std::enable_if_t<stdx::is_detected_v<HasIsLazyDecorationOp, T>>> = true;

/** Zero-initializables are not lazy */
template <typename T>
constexpr inline bool allowLazy<T, std::enable_if_t<std::is_arithmetic_v<T>>> = false;
template <typename T>
constexpr inline bool allowLazy<T*> = false;

template <typename... Ts>
constexpr inline bool allowLazy<std::basic_string<Ts...>> = true;
template <typename... Ts>
constexpr inline bool allowLazy<std::list<Ts...>> = true;
template <typename... Ts>
constexpr inline bool allowLazy<std::vector<Ts...>> = true;
template <typename... Ts>
constexpr inline bool allowLazy<std::map<Ts...>> = true;
template <typename... Ts>
constexpr inline bool allowLazy<std::unique_ptr<Ts...>> = true;
template <typename T>
constexpr inline bool allowLazy<std::shared_ptr<T>> = true;
template <typename T>
constexpr inline bool allowLazy<boost::optional<T>> = true;

/** These wrappers are lazy if their wrapped type is. */
template <typename T>
constexpr inline bool allowLazy<AtomicWord<T>> = allowLazy<T>;
template <typename T>
constexpr inline bool allowLazy<std::atomic<T>> = allowLazy<T>;  // NOLINT

/** BSONObj is lazy */
template <>
constexpr inline bool allowLazy<BSONObj> = true;


/**
 * Can't use the usual mongo::SpinLock as we can't rely on the usual
 * std::atomic_flag init being ok with all-zeros.
 */
class LazyInitFlag {
private:
    // zero fill == disengaged
    enum class State { empty, busy, done };

public:
    /**
     * Compete for the right to initialize the guarded object.
     *
     * - If true is returned, the caller has entered the "critical section". It
     *   must initialize the guarded object and call `initFinish()`.
     *
     * - Otherwise, another caller has won, and false is returned. In that case,
     *   this call doesn't return until the winning caller calls `initFinish()`.
     */
    bool tryInitStart() {
        auto state = _state.load(std::memory_order_acquire);
        while (true) {
            if (state == State::done)
                return false;
            if (state == State::empty) {
                if (!_state.compare_exchange_strong(state, State::busy, std::memory_order_acq_rel))
                    continue;  // Lost. Reconsider from new state.
                return true;
            }
            if (state == State::busy) {
                // Lost. Wait for the winner. This kind of race should be
                // extremely unlikely in the first place. We're default
                // constructing, so a quick spin should be sufficient most of
                // the time. If we are still busy after this short number
                // of spins, the loop becomes a core yield.
                int spins = 1000;
                do {
                    if (!spins)
                        _spinSlow();
                    else
                        --spins;
                    state = _state.load(std::memory_order_acquire);
                } while (state == State::busy);
                return false;
            }
        }
    }

    void initFinish() {
        _state.store(State::done, std::memory_order_release);
    }

    static void _spinSlow() {
#ifndef _MSC_VER
        MONGO_YIELD_CORE_FOR_SMT();
#else
        std::this_thread::yield();  // Punt: macro broken on Windows.
#endif
    }

    bool hasValue() const {
        return _state.load(std::memory_order_acquire) == State::done;
    }

    bool relaxedHasValue() const {
        return _state.load(std::memory_order_relaxed) == State::done;
    }

private:
    std::atomic<State> _state;  // NOLINT
};

/**
 * A wrapper that defers construction of its `value` until first access.
 * Unlike `optional`, it is known to be empty if represented by all zeros.
 * Like an "autovivifying" `optional`.
 *
 * The default constructor of `T` must not throw. For simplicity's sake, it
 * should also have no side effects, as the lazy initialization time can be
 * difficult to reason about.
 */
template <typename T>
class LazyInit {
public:
    using value_type = T;

    LazyInit() = default;

    ~LazyInit() {
        if constexpr (!std::is_trivially_destructible_v<value_type>)
            if (_flag.relaxedHasValue())
                value().~T();
    }

    LazyInit(const LazyInit& o) {
        if (o._flag.hasValue())
            value() = o.value();
    }

    LazyInit& operator=(const LazyInit& o) {
        if (this != &o)
            if (_flag.hasValue() || o._flag.hasValue())
                value() = o.value();
        return *this;
    }

    const value_type& value() const noexcept {
        if constexpr (!std::is_trivially_default_constructible_v<value_type>)
            _ensureValue();
        return *_ptr();
    }
    value_type& value() noexcept {
        return const_cast<T&>(std::as_const(*this).value());
    }

    static constexpr ptrdiff_t offsetOfValue() noexcept {
        return offsetof(LazyInit, _buf);
    }

private:
    /** Emplaces the value on first use. Optimized for happy path. */
    void _ensureValue() const noexcept {
        if (_flag.tryInitStart()) {
            new (const_cast<value_type*>(_ptr())) value_type{};
            _flag.initFinish();
        }
    }

    const value_type* _ptr() const {
        return reinterpret_cast<const value_type*>(_buf.data());
    }
    value_type* _ptr() {
        return const_cast<value_type*>(std::as_const(*this)._ptr());
    }

    mutable LazyInitFlag _flag;
    alignas(value_type) mutable std::array<char, sizeof(value_type)> _buf;
};

using CtorFn = void(void*);
using DtorFn = void(void*);
using CopyFn = void(void*, const void*);
using AssignFn = void(void*, const void*);

template <typename T>
struct BasicBoxingTraits {
    constexpr CtorFn* getConstructorFn() const {
        if constexpr (!std::is_trivially_constructible_v<T>)
            return +[](void* p) {
                new (p) T{};
            };
        return nullptr;
    }

    constexpr DtorFn* getDestructorFn() const {
        if constexpr (!std::is_trivially_destructible_v<T>)
            return +[](void* p) {
                static_cast<T*>(p)->~T();
            };
        return nullptr;
    }

    template <bool needCopy>
    static constexpr CopyFn* getCopyFn() {
        if constexpr (needCopy)
            return +[](void* p, const void* src) {
                new (p) T(*static_cast<const T*>(src));
            };
        return nullptr;
    }

    template <bool needCopy>
    static constexpr AssignFn* getAssignFn() {
        if constexpr (needCopy)
            return +[](void* p, const void* src) {
                *static_cast<T*>(p) = *static_cast<const T*>(src);
            };
        return nullptr;
    }

    static constexpr size_t boxAlignment() {
        return alignof(T);
    }

    static constexpr size_t boxSize() {
        return sizeof(T);
    }

    static constexpr const std::type_info* boxTypeInfo() {
        return &typeid(T);
    }

    const T* unbox(const void* boxAddress) const {
        return static_cast<const T*>(boxAddress);
    }

    static constexpr ptrdiff_t offsetOfValue() {
        return 0;
    }
};

// If a type <T> has the allowLazy=true trait, then it's boxed in a
// LazyInit<T>.
template <typename T>
constexpr auto decorationBoxingTraitsFor(std::type_identity<T>) {
    if constexpr (allowLazy<T>) {
        struct Traits : BasicBoxingTraits<LazyInit<T>> {
            constexpr CtorFn* getConstructorFn() {
                // Exploit the LazyInit property that doing nothing is a valid construction.
                return nullptr;
            }
            const T* unbox(const void* boxAddress) const {
                return &static_cast<const LazyInit<T>*>(boxAddress)->value();
            }
            constexpr ptrdiff_t offsetOfValue() {
                return LazyInit<T>::offsetOfValue();
            }
        };
        return Traits{};
    } else {
        struct Traits : BasicBoxingTraits<T> {};
        return Traits{};
    }
}

// Adl hook to make specializations easier to find.
template <typename T>
constexpr auto decorationBoxingTraitsFor() {
    return decorationBoxingTraitsFor(std::type_identity<T>{});
}

struct LifecycleOperations {
    CtorFn* ctorFn;     /** null if trivial */
    DtorFn* dtorFn;     /** null if trivial */
    CopyFn* copyFn;     /** null if no copy */
    AssignFn* assignFn; /** null if no assignment */
};

template <typename T, bool needCopy>
constexpr inline LifecycleOperations lifecycleOperations = [] {
    auto traits = decorationBoxingTraitsFor<T>();
    return LifecycleOperations{traits.getConstructorFn(),
                               traits.getDestructorFn(),
                               traits.template getCopyFn<needCopy>(),
                               traits.template getAssignFn<needCopy>()};
}();

/**
 * Encodes all of the properties and type-erased operations needed to work with
 * a value in a DecorationBufffer.
 */
class RegistryEntry {
public:
    RegistryEntry(const std::type_info* typeInfo,
                  ptrdiff_t offset,
                  const LifecycleOperations* ops,
                  size_t size,
                  size_t align)
        : _typeInfo{typeInfo}, _offset{offset}, _ops{ops}, _size{size}, _align{align} {}

    void construct(void* buf) const {
        if (const auto& ctor = _ops->ctorFn)
            ctor(_addOffset(buf));
    }

    void destroy(void* buf) const {
        if (const auto& dtor = _ops->dtorFn)
            dtor(_addOffset(buf));
    }

    void copy(void* buf, const void* srcBuf) const {
        invariant(_ops->copyFn);
        _ops->copyFn(_addOffset(buf), _addOffset(srcBuf));
    }

    void assign(void* buf, const void* srcBuf) const {
        invariant(_ops->assignFn);
        _ops->assignFn(_addOffset(buf), _addOffset(srcBuf));
    }

    ptrdiff_t offset() const {
        return _offset;
    }

    size_t size() const {
        return _size;
    }

    size_t align() const {
        return _align;
    }

private:
    const void* _addOffset(const void* buf) const {
        return static_cast<const char*>(buf) + _offset;
    }

    void* _addOffset(void* buf) const {
        return const_cast<void*>(_addOffset(const_cast<const void*>(buf)));
    }

    const std::type_info* _typeInfo;  // for gdb
    ptrdiff_t _offset;
    const LifecycleOperations* _ops;
    size_t _size;
    size_t _align;
};

class Registry {
public:
    /** Return registry position of new entry. */
    template <typename T>
    size_t declare(const LifecycleOperations* ops) {
        auto traits = decorationBoxingTraitsFor<T>();
        static constexpr auto al = traits.boxAlignment();
        static constexpr auto sz = traits.boxSize();
        static_assert(al <= alignof(std::max_align_t), "over-aligned decoration");
        ptrdiff_t offset = (_bufferSize + al - 1) / al * al;  // pad to alignment
        _entries.push_back({traits.boxTypeInfo(), offset, ops, sz, al});
        _bufferSize = offset + sz;
        return _entries.size() - 1;
    }

    size_t bufferSize() const {
        return _bufferSize;
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
    std::vector<RegistryEntry> _entries;
    size_t _bufferSize = sizeof(void*);  // The owner pointer is always present.
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
private:
    static constexpr auto _boxingTraits() {
        return decorationBoxingTraitsFor<T>();
    }

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
        const void* boxAddress = _decorationToBoxAddress(&t);
        const void* blockAddress = static_cast<const char*>(boxAddress) - _offsetInBlock();
        auto op =
            DecoratedType::downcastBackLink(*reinterpret_cast<const void* const*>(blockAddress));
        invariant(&(*op)[*this] == &t, "Inconsistent deco => owner => deco round trip");
        return *op;
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

    /**
     * Translate the whole decorations buffer address into a `T*`.
     * This means finding out where in `buf` the "box" is, and
     * then where in the "box" the value `T*` is. A DecorationToken
     * has access to all necessary information to do this.
     */
    const T* getValue(const void* buf) const {
        // The decoration value is in a "box", and that box
        // address needs to be converted into a `T*`.
        return _boxingTraits().unbox(static_cast<const char*>(buf) + _offsetInBlock());
    }

private:
    ptrdiff_t _offsetInBlock() const {
        return (*_registry)[_registryPosition].offset();
    }

    const void* _decorationToBoxAddress(const void* p) const {
        return static_cast<const char*>(p) - _boxingTraits().offsetOfValue();
    }

    const Registry* _registry = &decorable_detail::getRegistry<DecoratedType>();
    size_t _registryPosition;
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
            for (; i != n; ++i)
                reg[i].copy(_data, other._data);
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
        auto& reg = _reg();
        size_t n = reg.size();
        for (size_t i = 0; i != n; ++i)
            reg[i].assign(_data, other._data);
        return *this;
    }

    template <typename T>
    const T* getAtDecorationToken(const DecorationToken<DecoratedType, T>& deco) const {
        return deco.getValue(_data);
    }

private:
    static Registry& _reg() {
        return getRegistry<DecoratedType>();
    }

    void _constructorCommon() {
        auto& reg = _reg();
        size_t n = reg.size();
        size_t i = 0;
        try {
            for (; i != n; ++i)
                reg[i].construct(_data);
        } catch (...) {
            _tearDownParts(i);
            throw;
        }
    }

    void _tearDownParts(size_t count) noexcept {
        auto& reg = _reg();
        while (count--)
            reg[count].destroy(_data);
    }

    template <typename DecoratedBase>
    void _setBackLink(const DecoratedBase* decorated) {
        static_assert(std::is_base_of_v<DecoratedBase, DecoratedType>);
        *reinterpret_cast<const void**>(_data) = DecoratedType::upcastBackLink(decorated);
    }

    std::unique_ptr<unsigned char[]> _makeData() {
        return std::make_unique<unsigned char[]>(_reg().bufferSize());
    }

    std::unique_ptr<unsigned char[]> _dataOwnership{_makeData()};
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
        return *_decorations.getAtDecorationToken(deco);
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
