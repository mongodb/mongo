// clonable_ptr.h

/*-
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>

namespace mongo {
namespace clonable_ptr_detail {
// This is the default `CloneFactory` conforming to `mongo::concept::CloneFactory` for
// `clonable_ptr`.
template <typename Clonable>
struct CloneFactory {
    auto operator()(const Clonable& c) const -> decltype(c.clone()) {
        return c.clone();
    }
};

// TODO: Move some of these traits detection structs to a template metaprogramming header.
template <typename T>
struct detect_clone_factory_type_member_impl {
    struct Fallback {
        struct clone_factory_type {};
    };

    struct Derived : T, Fallback {};

    using Yes = char[2];
    using No = char[1];

    template <typename U>
    static No& test(typename U::clone_factory_type*);

    template <typename U>
    static Yes& test(U*);

    static constexpr bool value = sizeof(test<Derived>(0)) == sizeof(Yes);

    using type = typename std::integral_constant<bool, value>::type;
};

template <typename T>
struct detect_clone_factory_type_member : std::conditional<std::is_class<T>::value,
                                                           detect_clone_factory_type_member_impl<T>,
                                                           std::false_type>::type {};

template <typename T, bool has_clone_factory_member = detect_clone_factory_type_member<T>::value>
struct clonable_traits_impl;

template <typename T>
struct clonable_traits_impl<T, false> {
    using clone_factory_type = CloneFactory<T>;
};

template <typename T>
struct clonable_traits_impl<T, true> {
    using clone_factory_type = typename T::clone_factory_type;
};
}  // namespace clonable_ptr_detail

/**
 * The 'clonable_traits' class is a specializable traits class for clonable-like types.  By
 * specializing this traits class for a type it is possible to change the global default
 * `CloneFactory` type for a specific type.  Types which conform to `mongo::concept::Clonable`
 * will get a default `CloneFactory` type whch invokes their specific `Clonable::clone` function.  A
 * specialization can be used to make a type use a different clone factory function.  A type `T` may
 * specify `T::clone_factory_type` instead of specializing this traits type.
 */
template <typename T>
struct clonable_traits : clonable_ptr_detail::clonable_traits_impl<T> {};

/**
 * The `clonable_ptr` represents a value-like type held at a distance.  The `clonable_ptr` class is
 * a smart-pointer type which functions like a `std::unique_ptr` with the added ability to create
 * new copies of the pointee on copy construction.  The default CloneFactory assumes that `T` is a
 * type which models the Concept `mongo::concept::Clonable`.  The supplied type may supply an
 * alternative default `CloneFactory` type by either of two means:
 *
 *  * `T` may define a member `T::clone_factory_type` which conforms to
 *    `mongo::concept::CloneFactory`
 *  * `T` may have an accompanying specialization of `mongo::clonable_traits< T >` which
 *    defines `clonable_factory_type`.
 *
 * NOTE: The `CloneFactory` type is permitted to be stateful, but must be copy constructible and
 * copy assignable.
 * NOTE: The `CloneFactory` member does NOT participate in value comparisons for a `clonable_ptr`,
 * even when it has state.
 *
 * `T`: The type of the object being managed.
 * `CloneFactory`: A type which models the Concept `mongo::concept::CloneFactory`.
 * `UniquePtr`: A type which models the Concept `mongo::concept::UniquePtr`
 */
template <typename T,
          typename CloneFactory = typename clonable_traits<T>::clone_factory_type,
          template <typename, typename...> class UniquePtr = std::unique_ptr>
class clonable_ptr {
private:
    // `std::tuple` is used to avoid allocating storage for `cloneFactory` if it is a non-storage
    // type.
    std::tuple<CloneFactory, UniquePtr<T>> data;

    inline const CloneFactory& cloneFactory() const {
        return std::get<0>(data);
    }

    inline const UniquePtr<T>& ptr() const {
        return std::get<1>(data);
    }

    inline UniquePtr<T>& ptr() {
        return std::get<1>(data);
    }

    inline const auto& _makeEqualityLens() const noexcept {
        return this->ptr();
    }

    inline const auto& _makeStrictWeakOrderLens() const noexcept {
        return this->ptr();
    }

    static inline UniquePtr<T> clone_with_factory_impl(const T& copy, const CloneFactory& factory) {
        return UniquePtr<T>{factory(copy)};
    }

    template <typename Pointerlike>
    static inline UniquePtr<T> clone_with_factory(Pointerlike&& copy, const CloneFactory& factory) {
        if (!copy)
            return nullptr;
        return clone_with_factory_impl(*copy, factory);
    }

    struct internal_construction {};

    explicit inline clonable_ptr(UniquePtr<T>&& p,
                                 const CloneFactory* const f,
                                 const internal_construction&)
        : data(*f, std::move(p)) {}

    explicit inline clonable_ptr(UniquePtr<T>&& p, CloneFactory&& f, const internal_construction&)
        : data(std::move(f), std::move(p)) {}

public:
    /*! Destroys this pointer.  Functions like `std::unique_ptr`. */
    inline ~clonable_ptr() noexcept = default;

    /*! Moves a value, by pointer.  Functions like `std::unique_ptr`. */
    inline clonable_ptr(clonable_ptr&&) noexcept(
        noexcept(CloneFactory{std::declval<CloneFactory>()}) &&
        noexcept(UniquePtr<T>{std::declval<UniquePtr<T>>()})) = default;

    /*! Moves a value, by pointer.  Functions like `std::unique_ptr`. */
    inline clonable_ptr& operator=(clonable_ptr&&) &
        noexcept(noexcept(std::declval<CloneFactory>() = std::declval<CloneFactory>()) &&
                 noexcept(std::declval<UniquePtr<T>>() = std::declval<UniquePtr<T>>())) = default;

    /*!
     * Constructs a pointer referring to a new copy of an original value.  The old object owned by
     * `*this` will be deleted, and `*this` will manage a new copy of `copy`, as created by
     * `copy->clone()`.  If `copy` is not managing anything (its internal pointer is `nullptr`),
     * then this new copy will also be nullptr.
     *
     * POST: `copy != nullptr ? copy != *this : copy == *this` -- If `copy` stores a pointer to a
     * value, then `*this` will have an independent pointer.  If `copy` stores `nullptr`, then
     * `*this` will also store `nullptr`.
     *
     * `copy`: The original value to copy.
     * THROWS: Any exceptions thrown by `cloneFactory( *copy )`.
     * TODO: Consider adding a noexcept deduction specifier to this copy operation.
     */
    inline clonable_ptr(const clonable_ptr& copy)
        : data{copy.cloneFactory(), clone_with_factory(copy, copy.cloneFactory())} {}

    /*!
     * Constructs a pointer referring to a new copy of an original value.  The old object owned by
     * `*this` will be deleted, and `*this` will manage a new copy of `copy`, as created by
     * `copy->clone()`.  If `copy` is not managing anything (its internal pointer is `nullptr`),
     * then this new copy will also be nullptr.
     *
     * POST: `copy != nullptr ? copy != *this : copy == *this` -- If `copy` stores a pointer to a
     * value, then `*this` will have an independent pointer.  If `copy` stores `nullptr`, then
     * `*this` will also store `nullptr`.
     *
     * NOTE: The `CloneFactory` will be copied from the `copy` poiner, by default.
     *
     * `copy`: The original value to copy.
     * `factory`: The factory to use for cloning.  Defaults to the source's factory.
     * THROWS: Any exceptions thrown by `factory( *copy )`.
     * TODO: Consider adding a noexcept deduction specifier to this copy operation.
     */
    inline clonable_ptr(const clonable_ptr& copy, const CloneFactory& factory)
        : data{factory, clone_with_factory(copy, factory)} {}

    /*!
     * Changes the value of this pointer, by creating a new object having the same value as `copy`.
     * The old object owned by `*this` will be deleted, and `*this` will manage a new copy of
     * `copy`, as created by `copy->clone()`.  If `copy` is not managing anything (its internal
     * pointer is `nullptr`), then this new copy will also be nullptr.
     *
     * NOTE: This operation cannot be conducted on an xvalue or prvalue instance.  (This prevents
     * silliness such as: `func_returning_ptr()= some_other_func_returning_ptr();`)
     *
     * NOTE: `copy`'s `CloneFactory` will be used to copy.
     *
     * POST: `copy != nullptr ? copy != *this : copy == *this` -- If `copy` stores a pointer to a
     * value, then `*this` will have an independent pointer.  If `copy` stores `nullptr`, then
     * `*this` will also store `nullptr`.
     *
     * `copy`: The value to make a copy of.
     * RETURNS: A reference to this pointer, after modification.
     * TODO: Consider adding a noexcept deduction specifier to this copy operation.
     */
    inline clonable_ptr& operator=(const clonable_ptr& copy) & {
        return *this = clonable_ptr{copy};
    }

    // Maintenance note: The two enable_if overloads of `clonable_ptr( std::nullptr_t )` are
    // necessary, due to the fact that `std::nullptr_t` is capable of implicit conversion to a
    // built-in pointer type.  If the stateful form being deleted causes the `nullptr` to convert,
    // this could cause binding to another ctor which may be undesired.

    /*!
     * `nullptr` construct a clonable pointer (to `nullptr`), if the `CloneFactory` type is
     * stateless.
     * The value will be a pointer to nothing, with a default `CloneFactory`.
     * NOTE: This constructor is only available for types with a stateless `CloneFactory` type.
     */
    template <typename CloneFactory_ = CloneFactory>
    inline clonable_ptr(
        typename std::enable_if<std::is_empty<CloneFactory_>::value, std::nullptr_t>::type) {}

    /*!
     * Disable `nullptr` construction of clonable pointer (to `nullptr`), if the `CloneFactory` type
     * is stateful.
     * NOTE: This constructor is disabled for types with a stateless `CloneFactory` type.
     */
    template <typename CloneFactory_ = CloneFactory>
    inline clonable_ptr(typename std::enable_if<!std::is_empty<CloneFactory_>::value,
                                                std::nullptr_t>::type) = delete;

    /*!
     * Constructs a pointer to nothing, with a default `CloneFactory`.
     * This function is unavailable when `CloneFactory` is stateful.
     */
    template <typename CloneFactory_ = CloneFactory,
              typename = typename std::enable_if<std::is_empty<CloneFactory_>::value>::type>
    explicit inline clonable_ptr() noexcept {}

    /*! Constructs a pointer to nothing, with the specified `CloneFactory`. */
    explicit inline clonable_ptr(CloneFactory factory) : data{factory, nullptr} {}

    /*!
     * Constructs a `clonable_ptr` which owns `p`, initializing the stored pointer with `p`.
     * This function is unavailable when `CloneFactory` is stateful.
     * `p`: The pointer to take ownership of.
     */
    template <typename CloneFactory_ = CloneFactory>
    explicit inline clonable_ptr(
        typename std::enable_if<std::is_empty<CloneFactory_>::value, T* const>::type p)
        : clonable_ptr(UniquePtr<T>{p}) {}

    /*!
     * Disable single-argument construction of clonable pointer (with a raw pointer), if the
     * `CloneFactory` type is stateful.
     * NOTE: This constructor is disabled for types with a stateless `CloneFactory` type.
     */
    template <typename CloneFactory_ = CloneFactory>
    explicit inline clonable_ptr(
        typename std::enable_if<!std::is_empty<CloneFactory_>::value, T* const>::type) = delete;

    // The reason that we have two overloads for clone factory is to ensure that we avoid as many
    // exception-unsafe uses as possible.  The const-lvalue-reference variant in conjunction with
    // the rvalue-reference variant lets us separate the cases of "constructed in place" from
    // "passed from a local".  In the latter case, we can't make our type any safer, since the
    // timing of the construction of the local and the timing of the `new` on the raw pointer are
    // out of our control.  At least we prevent an accidental use which SEEMS exception safe but
    // isn't -- hopefully highlighting exception unsafe code, by making it more explicit.  In the
    // former, "constructed in place", case, we are able to successfully move construct without
    // exception problems, if it's nothrow move constructible.  If it isn't we flag a compiler
    // error.  In this case, too, we prevent accidental use which SEEMS exception safe and hopefully
    // will similarly highlight exception unsafe code.

    /*!
     * Constructs a `clonable_ptr` which owns `p`, initializing the stored pointer with `p`.  The
     * `factory` parameter will be used as the `CloneFactory`
     * `p`: The pointer to take ownership of.
     * `factory`: The clone factory to use in future copies.
     * NOTE: It is not recommended to use this constructor, as the following is not exception safe
     * code:
     * ~~~
     * std::function<T* ()> cloner= [](const T& p){ return p; };
     * auto getCloner= [=]{ return cloner; };
     * clonable_ptr<T, std::function<T* ()>> bad{new T, getCloner()}; // BAD IDEA!!!
     * ~~~
     * Even if the above could be made exception safe, there are other more complicated use cases
     * which would not be exception safe.  (The above is not exception safe, because the `new T`
     * expression can be evaluated before the `getCloner()` expression is evaluated.  `getCloner()`
     * is allowed to throw, thus leaving `new T` to be abandoned.
     */
    explicit inline clonable_ptr(T* const p, const CloneFactory& factory)
        : clonable_ptr{UniquePtr<T>{p}, std::addressof(factory), internal_construction{}} {}

    /*!
     * We forbid construction of a `clonable_ptr` from an unmanaged pointer, when specifying
     * a cloning function -- regardless of whether the `CloneFactory` is stateful or not.
     * NOTE: We have disabled this constructor, as the following is not exception safe
     * code:
     * ~~~
     * clonable_ptr<T, std::function<T* ()>> bad{new T, [](const T& p){ return p; }}; // BAD IDEA!!!
     * ~~~
     * Even if the above could be made exception safe, there are other more complicated use cases
     * which would not be exception safe.  (The above is not exception safe, because the `new T`
     * expression can be evaluated before the lambda expression is evaluated and converted to a
     * `std::function`.  The `std::function` constructor is allowed to throw, thus leaving `new T`
     * to be abandoned.  More complicated cases are completely hidden from `clonable_ptr`'s
     * inspection, thus making this constructor too dangerous to exist.
     */
    explicit inline clonable_ptr(T* const p, CloneFactory&& factory) = delete;

    /*!
     * Constructs a `nullptr` valued clonable pointer, with a specified `CloneFactory`, `factory`.
     */
    explicit inline clonable_ptr(std::nullptr_t, CloneFactory&& factory)
        : clonable_ptr{UniquePtr<T>{nullptr}, std::move(factory), internal_construction{}} {}

    /*!
     * Constructs a `clonable_ptr` by transferring ownership from `p` to `*this`.  A default
     * `CloneFactory` will be provided for future copies.
     * `p`: The pointer to take ownership of.
     * NOTE: This constructor allows for implicit conversion from a `UniquePtr` (xvalue) object.
     * NOTE: This constructor is unavailable when `CloneFactory` is stateful.
     * NOTE: This usage should be preferred over the raw-pointer construction forms, when using
     * factories as constructor arguments, as in the following exception safe code:
     * ~~~
     * clonable_ptr<T, std::function<T* ()>> good{std::make_unique<T>(),
     *                                            [](const T& p){ return p; }}; // GOOD IDEA!!!
     * ~~~
     */
    template <typename CloneFactory_ = CloneFactory,
              typename Derived,
              typename = typename std::enable_if<std::is_empty<CloneFactory_>::value>::type>
    inline clonable_ptr(UniquePtr<Derived> p) : data{CloneFactory{}, std::move(p)} {}

    /*!
     * Constructs a `clonable_ptr` by transferring ownership from `p` to `*this`.  The `factory`
     * parameter will be used as the `CloneFactory` for future copies.
     * NOTE: This constructor allows for implicit conversion from a `UniquePtr` (xvalue) object.
     * `p`: The pointer to take ownership of.
     * `factory`: The clone factory to use in future copies.
     * NOTE: This usage should be preferred over the raw-pointer construction forms, when using
     * factories as constructor arguments, as in the following exception safe code:
     * ~~~
     * clonable_ptr<T, std::function<T* ()>> good{std::make_unique<T>(),
     *                                            [](const T& p){ return p; }}; // GOOD IDEA!!!
     * ~~~
     */
    template <typename Derived>
    inline clonable_ptr(UniquePtr<Derived> p, CloneFactory factory)
        : data{std::move(factory), std::move(p)} {}

    /*!
     * Changes the value of this pointer, by creating a new object having the same value as `copy`.
     * The old object owned by `*this` will be deleted, and `*this` will manage a new copy of
     * `copy`, as created by `copy->clone()`.  If `copy` is not managing anything (its internal
     * pointer is `nullptr`), then this new copy will also be nullptr.
     *
     * NOTE: This operation cannot be performed on an xvalue or prvalue instance.  (This prevents
     * silliness such as: `func_returning_ptr()= some_other_func_returning_ptr();`)
     *
     * NOTE: `copy`'s `CloneFactory` will be used to copy.
     *
     * POST: `copy != nullptr ? copy != *this : copy == *this` -- If `copy` stores a pointer to a
     * value, then `*this` will have an independent pointer.  If `copy` stores `nullptr`, then
     * `*this` will also store `nullptr`.
     *
     * `copy`: The value to make a copy of.
     * RETURNS: A reference to this pointer, after modification.
     */
    inline clonable_ptr& operator=(UniquePtr<T> copy) & {
        return *this = std::move(clonable_ptr{std::move(copy), this->cloneFactory()});
    }

    template <typename Derived>
    inline clonable_ptr& operator=(UniquePtr<Derived> copy) & {
        return *this = std::move(clonable_ptr{std::move(copy), this->cloneFactory()});
    }

    /*!
     * Change the `CloneFactory` for `*this` to `factory`.
     * NOTE: This operation cannot be performed on an xvalue or prvalue instance.  (This prevents
     * silliness such as: `func_returning_ptr().setCloneFactory( factory );`.)
     */
    template <typename FactoryType>
    inline void setCloneFactory(FactoryType&& factory) & {
        this->cloneFactory() = std::forward<FactoryType>(factory);
    }

    /*!
     * Dereferences the pointer owned by `*this`.
     * NOTE: The behavior is undefined if `this->get() == nullptr`.
     * RETURNS: The object owned by `*this`, equivalent to `*get()`.
     */
    inline auto& operator*() const {
        return *this->ptr();
    }

    /*!
     * Dereferences the pointer owned by `*this`.
     * NOTE: The behavior is undefined if `this->get() == nullptr`.
     * RETURNS: A pointer to the object owned by `*this`, equivalent to `get()`.
     */
    inline auto* operator-> () const {
        return this->ptr().operator->();
    }

    /*!
     * Returns `true` if `*this` owns a pointer to a value, and `false` otherwise.
     * RETURNS: A value equivalent to `static_cast< bool >( this->get() )`.
     */
    explicit inline operator bool() const noexcept {
        return this->ptr().get();
    }

    /*!
     * Converts `*this` to a `UniquePtr< T >` by transferring ownership.  This function will retire
     * ownership of the pointer owned by `*this`.  This is a safe operation, as this function cannot
     * be called from an lvalue context -- rvalue operations are used to represent transfer of
     * ownership semantics.
     *
     * NOTE: This function is only applicable in `rvalue` contexts.
     * NOTE: This function has transfer of ownership semantics.
     *
     * RETURNS: A `UniquePtr< T >` which owns the pointer formerly managed by `*this`.
     */
    inline operator UniquePtr<T>() && {
        return std::move(this->ptr());
    }

    /*! Provides a constant `UniquePtr< T >` view of the object owned by `*this`. */
    inline operator const UniquePtr<T>&() const& {
        return this->ptr();
    }

    /*! Provides a mutable `UniquePtr< T >` view of the object owned by `*this`. */
    inline operator UniquePtr<T>&() & {
        return this->ptr();
    }

    /*! Provides a C-style `T *` pointer to the object owned by `*this`. */
    inline T* get() const {
        return this->ptr().get();
    }

    inline void reset() & noexcept {
        this->ptr().reset();
    }

    inline void reset(T* const p) & {
        this->ptr().reset(p);
    }

    // Equality

    inline friend bool operator==(const clonable_ptr& lhs, const clonable_ptr& rhs) {
        return lhs._makeEqualityLens() == rhs._makeEqualityLens();
    }

    template <template <typename, typename...> class U, typename... UArgs>
    inline friend bool operator==(const U<T, UArgs...>& lhs, const clonable_ptr& rhs) {
        return lhs == rhs._makeEqualityLens();
    }

    template <template <typename, typename...> class U, typename... UArgs>
    inline friend bool operator==(const clonable_ptr& lhs, const U<T, UArgs...>& rhs) {
        return lhs._makeEqualityLens() == rhs;
    }

    inline friend bool operator==(const std::nullptr_t& lhs, const clonable_ptr& rhs) {
        return lhs == rhs._makeEqualityLens();
    }

    inline friend bool operator==(const clonable_ptr& lhs, const std::nullptr_t& rhs) {
        return lhs._makeEqualityLens() == rhs;
    }

    // Strict weak order

    inline friend bool operator<(const clonable_ptr& lhs, const clonable_ptr& rhs) {
        return lhs._makeStrictWeakOrderLens() < rhs._makeStrictWeakOrderLens();
    }

    template <template <typename, typename...> class U, typename... UArgs>
    inline friend bool operator<(const U<T, UArgs...>& lhs, const clonable_ptr& rhs) {
        return lhs < rhs._makeStrictWeakOrderLens();
    }

    template <template <typename, typename...> class U, typename... UArgs>
    inline friend bool operator<(const clonable_ptr& lhs, const U<T, UArgs...>& rhs) {
        return lhs._makeStrictWeakOrderLens() < rhs;
    }

    inline friend bool operator<(const std::nullptr_t& lhs, const clonable_ptr& rhs) {
        return lhs < rhs._makeStrictWeakOrderLens();
    }

    inline friend bool operator<(const clonable_ptr& lhs, const std::nullptr_t& rhs) {
        return lhs._makeStrictWeakOrderLens() < rhs;
    }
};

// Inequality

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator!=(const clonable_ptr<C, F, U>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs == rhs);
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator!=(const U<C, UArgs...>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs == rhs);
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator!=(const clonable_ptr<C, F, U>& lhs, const U<C, UArgs...>& rhs) {
    return !(lhs == rhs);
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator!=(const std::nullptr_t& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs == rhs);
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator!=(const clonable_ptr<C, F, U>& lhs, const std::nullptr_t& rhs) {
    return !(lhs == rhs);
}

// Greater than

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator>(const clonable_ptr<C, F, U>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return rhs < lhs;
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator>(const U<C, UArgs...>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return rhs < lhs;
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator>(const clonable_ptr<C, F, U>& lhs, const U<C, UArgs...>& rhs) {
    return rhs < lhs;
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator>(const std::nullptr_t& lhs, const clonable_ptr<C, F, U>& rhs) {
    return rhs < lhs;
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator>(const clonable_ptr<C, F, U>& lhs, const std::nullptr_t& rhs) {
    return rhs < lhs;
}

// Equal or Less

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator<=(const clonable_ptr<C, F, U>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs > rhs);
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator<=(const U<C, UArgs...>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs > rhs);
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator<=(const clonable_ptr<C, F, U>& lhs, const U<C, UArgs...>& rhs) {
    return !(lhs > rhs);
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator<=(const std::nullptr_t& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs > rhs);
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator<=(const clonable_ptr<C, F, U>& lhs, const std::nullptr_t& rhs) {
    return !(lhs > rhs);
}

// Equal or greater

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator>=(const clonable_ptr<C, F, U>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs < rhs);
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator>=(const U<C, UArgs...>& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs < rhs);
}

template <typename C, typename F, template <typename, typename...> class U, typename... UArgs>
inline bool operator>=(const clonable_ptr<C, F, U>& lhs, const U<C, UArgs...>& rhs) {
    return !(lhs < rhs);
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator>=(const std::nullptr_t& lhs, const clonable_ptr<C, F, U>& rhs) {
    return !(lhs < rhs);
}

template <typename C, typename F, template <typename, typename...> class U>
inline bool operator>=(const clonable_ptr<C, F, U>& lhs, const std::nullptr_t& rhs) {
    return !(lhs < rhs);
}
}  // namespace mongo
