/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2022-2024 Andrey Semashev
 */
/*!
 * \file scope/defer.hpp
 *
 * This header contains definition of \c defer_guard template.
 */

#ifndef BOOST_SCOPE_DEFER_HPP_INCLUDED_
#define BOOST_SCOPE_DEFER_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/is_not_like.hpp>
#include <boost/scope/detail/move_or_copy_construct_ref.hpp>
#include <boost/scope/detail/type_traits/conjunction.hpp>
#include <boost/scope/detail/type_traits/is_nothrow_invocable.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {

template< typename Func >
class defer_guard;

namespace detail {

// Workaround for clang < 5.0 which can't pass defer_guard as a template template parameter from within defer_guard definition
template< typename T >
using is_not_like_defer_guard = detail::is_not_like< T, defer_guard >;

} // namespace detail

/*!
 * \brief Defer guard that invokes a function upon leaving the scope.
 *
 * The scope guard wraps a function object callable with no arguments
 * that can be one of:
 *
 * \li A user-defined class with a public `operator()`.
 * \li An lvalue reference to such class.
 * \li An lvalue reference or pointer to function taking no arguments.
 *
 * The defer guard unconditionally invokes the wrapped function object
 * on destruction.
 */
template< typename Func >
class defer_guard
{
//! \cond
private:
    struct data
    {
        Func m_func;

        template< typename F, typename = typename std::enable_if< std::is_constructible< Func, F >::value >::type >
        explicit data(F&& func, std::true_type) noexcept :
            m_func(static_cast< F&& >(func))
        {
        }

        template< typename F, typename = typename std::enable_if< std::is_constructible< Func, F >::value >::type >
        explicit data(F&& func, std::false_type) try :
            m_func(static_cast< F&& >(func))
        {
        }
        catch (...)
        {
            func();
        }
    };

    data m_data;

//! \endcond
public:
    /*!
     * \brief Constructs a defer guard with a given callable function object.
     *
     * **Requires:** \c Func is constructible from \a func.
     *
     * **Effects:** If \c Func is nothrow constructible from `F&&` then constructs \c Func from
     *              `std::forward< F >(func)`, otherwise constructs from `func`.
     *
     *              If \c Func construction throws, invokes \a func before returning with the exception.
     *
     * **Throws:** Nothing, unless construction of the function object throws.
     *
     * \param func The callable function object to invoke on destruction.
     */
    template<
        typename F
        //! \cond
        , typename = typename std::enable_if< detail::conjunction<
            std::is_constructible<
                data,
                typename detail::move_or_copy_construct_ref< F, Func >::type,
                typename std::is_nothrow_constructible< Func, typename detail::move_or_copy_construct_ref< F, Func >::type >::type
            >,
            detail::is_not_like_defer_guard< F >
        >::value >::type
        //! \endcond
    >
    defer_guard(F&& func)
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(
            std::is_nothrow_constructible<
                data,
                typename detail::move_or_copy_construct_ref< F, Func >::type,
                typename std::is_nothrow_constructible< Func, typename detail::move_or_copy_construct_ref< F, Func >::type >::type
            >::value
        )) :
        m_data
        (
            static_cast< typename detail::move_or_copy_construct_ref< F, Func >::type >(func),
            typename std::is_nothrow_constructible< Func, typename detail::move_or_copy_construct_ref< F, Func >::type >::type()
        )
    {
    }

    defer_guard(defer_guard const&) = delete;
    defer_guard& operator= (defer_guard const&) = delete;

    /*!
     * \brief Invokes the wrapped callable function object and destroys the callable.
     *
     * **Throws:** Nothing, unless invoking the callable throws.
     */
    ~defer_guard() noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(detail::is_nothrow_invocable< Func& >::value))
    {
        m_data.m_func();
    }
};

#if !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)
template< typename Func >
defer_guard(Func) -> defer_guard< Func >;
#endif // !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)

} // namespace scope

//! \cond
#if defined(BOOST_MSVC)
#define BOOST_SCOPE_DETAIL_UNIQUE_VAR_TAG __COUNTER__
#else
#define BOOST_SCOPE_DETAIL_UNIQUE_VAR_TAG __LINE__
#endif
//! \endcond

/*!
 * \brief The macro creates a uniquely named defer guard.
 *
 * The macro should be followed by a function object that should be called
 * on leaving the current scope. Usage example:
 *
 * ```
 * BOOST_SCOPE_DEFER []
 * {
 *     std::cout << "Hello world!" << std::endl;
 * };
 * ```
 *
 * \note Using this macro requires C++17.
 */
#define BOOST_SCOPE_DEFER \
    boost::scope::defer_guard BOOST_JOIN(_boost_defer_guard_, BOOST_SCOPE_DETAIL_UNIQUE_VAR_TAG) =

} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_DEFER_HPP_INCLUDED_
