/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2022 Andrey Semashev
 */
/*!
 * \file scope/scope_success.hpp
 *
 * This header contains definition of \c scope_success template.
 */

#ifndef BOOST_SCOPE_SCOPE_SUCCESS_HPP_INCLUDED_
#define BOOST_SCOPE_SCOPE_SUCCESS_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/exception_checker.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/scope/detail/is_not_like.hpp>
#include <boost/scope/detail/type_traits/conjunction.hpp>
#include <boost/scope/detail/type_traits/is_invocable.hpp>
#include <boost/scope/detail/type_traits/is_nothrow_invocable.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {

template< typename Func, typename Cond >
class scope_success;

namespace detail {

// Workaround for clang < 5.0 which can't pass scope_success as a template template parameter from within scope_success definition
template< typename T >
using is_not_like_scope_success = detail::is_not_like< T, scope_success >;

template< typename Func >
class logical_not;

template< typename T >
using is_not_like_logical_not = detail::is_not_like< T, logical_not >;

template< typename Func >
class logical_not
{
public:
    using result_type = bool;

private:
    Func m_func;

public:
    template<
        bool Requires = std::is_default_constructible< Func >::value,
        typename = typename std::enable_if< Requires >::type
    >
    logical_not() noexcept(std::is_nothrow_default_constructible< Func >::value) :
        m_func()
    {
    }

    template<
        typename F,
        typename = typename std::enable_if< detail::conjunction<
            std::is_constructible< Func, F >,
            detail::is_not_like_logical_not< F >
        >::value >::type
    >
    explicit logical_not(F&& func) noexcept(std::is_nothrow_constructible< Func, F >::value) :
        m_func(static_cast< F&& >(func))
    {
    }

    result_type operator()() const noexcept(detail::is_nothrow_invocable< Func const& >::value)
    {
        return !m_func();
    }
};

} // namespace detail

/*!
 * \brief Scope exit guard that invokes a function upon leaving the scope, if
 *        a failure condition is not satisfied.
 *
 * The scope guard wraps two function objects: the scope guard action and
 * a failure condition for invoking the action. Both function objects must
 * be callable with no arguments and can be one of:
 *
 * \li A user-defined class with a public `operator()`.
 * \li An lvalue reference to such class.
 * \li An lvalue reference or pointer to function taking no arguments.
 *
 * The condition function object `operator()` must return a value
 * contextually convertible to \c true, if the failure is detected and the
 * action function object is not allowed to be executed, and \c false otherwise.
 * Additionally, the failure condition function object `operator()` must not
 * throw, as otherwise the action function object may not be called. If not
 * specified, the default failure condition checks whether the scope is left
 * due to an exception - the action function object will only be called if
 * the scope is left normally.
 *
 * \sa scope_exit
 * \sa scope_fail
 *
 * \tparam Func Scope guard action function object type.
 * \tparam Cond Scope guard failure condition function object type.
 */
template< typename Func, typename Cond = exception_checker >
class scope_success :
    public scope_exit< Func, detail::logical_not< Cond > >
{
//! \cond
private:
    using base_type = scope_exit< Func, detail::logical_not< Cond > >;

//! \endcond
public:
    /*!
     * \brief Constructs a scope guard with a given callable function object.
     *
     * **Requires:** \c Func is constructible from \a func. \c Cond is nothrow default-constructible.
     *
     * **Effects:** Constructs the scope guard as if by calling
     *              `scope_success(std::forward< F >(func), Cond(), active)`.
     *
     * **Throws:** Nothing, unless construction of the function objects throw.
     *
     * \param func The callable action function object to invoke on destruction.
     * \param active Indicates whether the scope guard should be active upon construction.
     *
     * \post `this->active() == active`
     */
    template<
        typename F
        //! \cond
        , typename = typename std::enable_if< detail::conjunction<
            std::is_constructible< base_type, F, bool >,
            detail::is_not_like_scope_success< F >
        >::value >::type
        //! \endcond
    >
    explicit scope_success(F&& func, bool active = true)
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(std::is_nothrow_constructible< base_type, F, bool >::value)) :
        base_type(static_cast< F&& >(func), active)
    {
    }

    /*!
     * \brief Constructs a scope guard with a given callable action and failure condition function objects.
     *
     * **Requires:** \c Func is constructible from \a func. \c Cond is constructible from \a cond.
     *
     * **Effects:** If \c Func is nothrow constructible from `F&&` then constructs \c Func from
     *              `std::forward< F >(func)`, otherwise constructs from `func`. If \c Cond is
     *              nothrow constructible from `C&&` then constructs \c Cond from
     *              `std::forward< C >(cond)`, otherwise constructs from `cond`.
     *
     *              If \c Func or \c Cond construction throws and \a active is \c true, invokes
     *              \a cond and, if it returns \c true, \a func before returning with the exception.
     *
     * **Throws:** Nothing, unless construction of the function objects throw.
     *
     * \param func The callable action function object to invoke on destruction.
     * \param cond The callable failure condition function object.
     * \param active Indicates whether the scope guard should be active upon construction.
     *
     * \post `this->active() == active`
     */
    template<
        typename F,
        typename C
        //! \cond
        , typename = typename std::enable_if< std::is_constructible< base_type, F, C, bool >::value >::type
        //! \endcond
    >
    explicit scope_success(F&& func, C&& cond, bool active = true)
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(std::is_nothrow_constructible< base_type, F, C, bool >::value)) :
        base_type(static_cast< F&& >(func), static_cast< C&& >(cond), active)
    {
    }

    /*!
     * \brief Move-constructs a scope guard.
     *
     * **Requires:** \c Func and \c Cond are nothrow move-constructible or copy-constructible.
     *
     * **Effects:** If \c Func is nothrow move-constructible then move-constructs \c Func from
     *              a member of \a that, otherwise copy-constructs \c Func. If \c Cond is nothrow
     *              move-constructible then move-constructs \c Cond from a member of \a that,
     *              otherwise copy-constructs \c Cond.
     *
     *              If \c Func or \c Cond construction throws and `that.active() == true`, invokes
     *              \c Cond object stored in \a that and, if it returns \c true, \a Func object
     *              (either the newly constructed one, if its construction succeeded, or the original
     *              one stored in \a that) before returning with the exception.
     *
     *              If the construction succeeds, marks \a that as inactive.
     *
     * **Throws:** Nothing, unless move-construction of the function objects throw.
     *
     * \param that Move source.
     *
     * \post `that.active() == false`
     */
    //! \cond
    template<
        bool Requires = std::is_move_constructible< base_type >::value,
        typename = typename std::enable_if< Requires >::type
    >
    //! \endcond
    scope_success(scope_success&& that)
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(std::is_nothrow_move_constructible< base_type >::value)) :
        base_type(static_cast< base_type&& >(that))
    {
    }

    scope_success& operator= (scope_success&&) = delete;

    scope_success(scope_success const&) = delete;
    scope_success& operator= (scope_success const&) = delete;
};

#if !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)
template< typename Func >
explicit scope_success(Func) -> scope_success< Func >;

template< typename Func >
explicit scope_success(Func, bool) -> scope_success< Func >;

template<
    typename Func,
    typename Cond,
    typename = typename std::enable_if< detail::is_invocable< Cond const& >::value >::type
>
explicit scope_success(Func, Cond) -> scope_success< Func, Cond >;

template<
    typename Func,
    typename Cond,
    typename = typename std::enable_if< detail::is_invocable< Cond const& >::value >::type
>
explicit scope_success(Func, Cond, bool) -> scope_success< Func, Cond >;
#endif // !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)

/*!
 * \brief Creates a scope fail guard with a given action function object.
 *
 * **Effects:** Constructs a scope guard as if by calling
 *              `scope_success< std::decay_t< F > >(std::forward< F >(func), active)`.
 *
 * \param func The callable function object to invoke on destruction.
 * \param active Indicates whether the scope guard should be active upon construction.
 */
template< typename F >
inline scope_success< typename std::decay< F >::type > make_scope_success(F&& func, bool active = true)
    noexcept(std::is_nothrow_constructible<
        scope_success< typename std::decay< F >::type >,
        F,
        bool
    >::value)
{
    return scope_success< typename std::decay< F >::type >(static_cast< F&& >(func), active);
}

/*!
 * \brief Creates a scope fail with given callable function objects.
 *
 * **Effects:** Constructs a scope guard as if by calling
 *              `scope_success< std::decay_t< F >, std::decay_t< C > >(
 *              std::forward< F >(func), std::forward< C >(cond), active)`.
 *
 * \param func The callable action function object to invoke on destruction.
 * \param cond The callable failure condition function object.
 * \param active Indicates whether the scope guard should be active upon construction.
 */
template< typename F, typename C >
inline
#if !defined(BOOST_SCOPE_DOXYGEN)
typename std::enable_if<
    detail::is_invocable< C const& >::value,
    scope_success< typename std::decay< F >::type, typename std::decay< C >::type >
>::type
#else
scope_success< typename std::decay< F >::type, typename std::decay< C >::type >
#endif
make_scope_success(F&& func, C&& cond, bool active = true)
    noexcept(std::is_nothrow_constructible<
        scope_success< typename std::decay< F >::type, typename std::decay< C >::type >,
        F,
        C,
        bool
    >::value)
{
    return scope_success< typename std::decay< F >::type, typename std::decay< C >::type >(static_cast< F&& >(func), static_cast< C&& >(cond), active);
}

} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_SCOPE_SUCCESS_HPP_INCLUDED_
