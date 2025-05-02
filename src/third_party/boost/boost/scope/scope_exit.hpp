/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/scope_exit.hpp
 *
 * This header contains definition of \c scope_exit template.
 */

#ifndef BOOST_SCOPE_SCOPE_EXIT_HPP_INCLUDED_
#define BOOST_SCOPE_SCOPE_EXIT_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/is_not_like.hpp>
#include <boost/scope/detail/compact_storage.hpp>
#include <boost/scope/detail/move_or_copy_construct_ref.hpp>
#include <boost/scope/detail/is_nonnull_default_constructible.hpp>
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
class scope_exit;

namespace detail {

// Workaround for clang < 5.0 which can't pass scope_exit as a template template parameter from within scope_exit definition
template< typename T >
using is_not_like_scope_exit = detail::is_not_like< T, scope_exit >;

//! The scope guard used to invoke the condition and action functions in case of exception during scope guard construction
template< typename Func, typename Cond >
class init_guard
{
private:
    Func& m_func;
    Cond& m_cond;
    bool m_active;

public:
    init_guard(Func& func, Cond& cond, bool active) noexcept :
        m_func(func),
        m_cond(cond),
        m_active(active)
    {
    }

    init_guard(init_guard const&) = delete;
    init_guard& operator= (init_guard const&) = delete;

    ~init_guard()
        noexcept(detail::conjunction<
            detail::is_nothrow_invocable< Func& >,
            detail::is_nothrow_invocable< Cond& >
        >::value)
    {
        if (m_active && m_cond())
            m_func();
    }

    Func&& get_func() noexcept
    {
        return static_cast< Func&& >(m_func);
    }

    Cond&& get_cond() noexcept
    {
        return static_cast< Cond&& >(m_cond);
    }

    void deactivate() noexcept
    {
        m_active = false;
    }
};

} // namespace detail

/*!
 * \brief A predicate that always returns \c true.
 *
 * This predicate can be used as the default condition function object for
 * \c scope_exit and similar scope guards.
 */
class always_true
{
public:
    //! Predicate result type
    using result_type = bool;

    /*!
     * **Throws:** Nothing.
     *
     * \returns \c true.
     */
    result_type operator()() const noexcept
    {
        return true;
    }
};

/*!
 * \brief Scope exit guard that conditionally invokes a function upon leaving the scope.
 *
 * The scope guard wraps two function objects: the scope guard action and
 * a condition for invoking the action. Both function objects must be
 * callable with no arguments and can be one of:
 *
 * \li A user-defined class with a public `operator()`.
 * \li An lvalue reference to such class.
 * \li An lvalue reference or pointer to function taking no arguments.
 *
 * The condition function object `operator()` must return a value
 * contextually convertible to \c true, if the action function object
 * is allowed to be executed, and \c false otherwise. Additionally,
 * the condition function object `operator()` must not throw, as
 * otherwise the action function object may not be called.
 *
 * The condition function object is optional, and if not specified in
 * template parameters, the scope guard will operate as if the condition
 * always returns \c true.
 *
 * The scope guard can be in either active or inactive state. By default,
 * the constructed scope guard is active. When active, and condition
 * function object returns \c true, the scope guard invokes the wrapped
 * action function object on destruction. Otherwise, the scope guard
 * does not call the wrapped action function object.
 *
 * The scope guard can be made inactive by moving-from the scope guard
 * or calling `set_active(false)`. An inactive scope guard can be made
 * active by calling `set_active(true)`. If a moved-from scope guard
 * is active on destruction, the behavior is undefined.
 *
 * \tparam Func Scope guard action function object type.
 * \tparam Cond Scope guard condition function object type.
 */
template< typename Func, typename Cond = always_true >
class scope_exit
{
//! \cond
private:
    struct func_holder :
        public detail::compact_storage< Func >
    {
        using func_base = detail::compact_storage< Func >;

        template<
            typename F,
            typename C,
            typename = typename std::enable_if< std::is_constructible< Func, F >::value >::type
        >
        explicit func_holder(F&& func, C&& cond, bool active, std::true_type) noexcept :
            func_base(static_cast< F&& >(func))
        {
        }

        template<
            typename F,
            typename C,
            typename = typename std::enable_if< std::is_constructible< Func, F >::value >::type
        >
        explicit func_holder(F&& func, C&& cond, bool active, std::false_type) :
            func_holder(detail::init_guard< F, C >(func, cond, active))
        {
        }

    private:
        template< typename F, typename C >
        explicit func_holder(detail::init_guard< F, C >&& init) :
            func_base(init.get_func())
        {
            init.deactivate();
        }
    };

    struct cond_holder :
        public detail::compact_storage< Cond >
    {
        using cond_base = detail::compact_storage< Cond >;

        template<
            typename C,
            typename = typename std::enable_if< std::is_constructible< Cond, C >::value >::type
        >
        explicit cond_holder(C&& cond, Func& func, bool active, std::true_type) noexcept :
            cond_base(static_cast< C&& >(cond))
        {
        }

        template<
            typename C,
            typename = typename std::enable_if< std::is_constructible< Cond, C >::value >::type
        >
        explicit cond_holder(C&& cond, Func& func, bool active, std::false_type) :
            cond_holder(detail::init_guard< Func&, C >(func, cond, active))
        {
        }

    private:
        template< typename C >
        explicit cond_holder(detail::init_guard< Func&, C >&& init) :
            cond_base(init.get_cond())
        {
            init.deactivate();
        }
    };

    struct data :
        public func_holder,
        public cond_holder
    {
        bool m_active;

        template<
            typename F,
            typename C,
            typename = typename std::enable_if< detail::conjunction<
                std::is_constructible< func_holder, F, C, bool, typename std::is_nothrow_constructible< Func, F >::type >,
                std::is_constructible< cond_holder, C, Func&, bool, typename std::is_nothrow_constructible< Cond, C >::type >
            >::value >::type
        >
        explicit data(F&& func, C&& cond, bool active)
            noexcept(detail::conjunction< std::is_nothrow_constructible< Func, F >, std::is_nothrow_constructible< Cond, C > >::value) :
            func_holder(static_cast< F&& >(func), static_cast< C&& >(cond), active, typename std::is_nothrow_constructible< Func, F >::type()),
            cond_holder(static_cast< C&& >(cond), func_holder::get(), active, typename std::is_nothrow_constructible< Cond, C >::type()),
            m_active(active)
        {
        }

        Func& get_func() noexcept
        {
            return func_holder::get();
        }

        Func const& get_func() const noexcept
        {
            return func_holder::get();
        }

        Cond& get_cond() noexcept
        {
            return cond_holder::get();
        }

        Cond const& get_cond() const noexcept
        {
            return cond_holder::get();
        }

        bool deactivate() noexcept
        {
            bool active = m_active;
            m_active = false;
            return active;
        }
    };

    data m_data;

//! \endcond
public:
    /*!
     * \brief Constructs a scope guard with a given callable action function object.
     *
     * **Requires:** \c Func is constructible from \a func. \c Cond is nothrow default-constructible
     *               and is not a pointer to function.
     *
     * \note The requirement for \c Cond default constructor to be non-throwing is to allow for
     *       the condition function object to be called in case if constructing either function
     *       object throws.
     *
     * **Effects:** Constructs the scope guard as if by calling
     *              `scope_exit(std::forward< F >(func), Cond(), active)`.
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
            detail::is_nothrow_nonnull_default_constructible< Cond >,
            std::is_constructible<
                data,
                typename detail::move_or_copy_construct_ref< F, Func >::type,
                typename detail::move_or_copy_construct_ref< Cond >::type,
                bool
            >,
            detail::is_not_like_scope_exit< F >
        >::value >::type
        //! \endcond
    >
    explicit scope_exit(F&& func, bool active = true)
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(
            std::is_nothrow_constructible<
                data,
                typename detail::move_or_copy_construct_ref< F, Func >::type,
                typename detail::move_or_copy_construct_ref< Cond >::type,
                bool
            >::value
        )) :
        m_data
        (
            static_cast< typename detail::move_or_copy_construct_ref< F, Func >::type >(func),
            static_cast< typename detail::move_or_copy_construct_ref< Cond >::type >(Cond()),
            active
        )
    {
    }

    /*!
     * \brief Constructs a scope guard with a given callable action and condition function objects.
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
     * \param cond The callable condition function object.
     * \param active Indicates whether the scope guard should be active upon construction.
     *
     * \post `this->active() == active`
     */
    template<
        typename F,
        typename C
        //! \cond
        , typename = typename std::enable_if< detail::conjunction<
            detail::is_invocable< C const& >,
            std::is_constructible<
                data,
                typename detail::move_or_copy_construct_ref< F, Func >::type,
                typename detail::move_or_copy_construct_ref< C, Cond >::type,
                bool
            >
        >::value >::type
        //! \endcond
    >
    explicit scope_exit(F&& func, C&& cond, bool active = true)
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(
            std::is_nothrow_constructible<
                data,
                typename detail::move_or_copy_construct_ref< F, Func >::type,
                typename detail::move_or_copy_construct_ref< C, Cond >::type,
                bool
            >::value
        )) :
        m_data
        (
            static_cast< typename detail::move_or_copy_construct_ref< F, Func >::type >(func),
            static_cast< typename detail::move_or_copy_construct_ref< C, Cond >::type >(cond),
            active
        )
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
        bool Requires = std::is_constructible<
            data,
            typename detail::move_or_copy_construct_ref< Func >::type,
            typename detail::move_or_copy_construct_ref< Cond >::type,
            bool
        >::value,
        typename = typename std::enable_if< Requires >::type
    >
    //! \endcond
    scope_exit(scope_exit&& that)
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(
            std::is_nothrow_constructible<
                data,
                typename detail::move_or_copy_construct_ref< Func >::type,
                typename detail::move_or_copy_construct_ref< Cond >::type,
                bool
            >::value
        )) :
        m_data
        (
            static_cast< typename detail::move_or_copy_construct_ref< Func >::type >(that.m_data.get_func()),
            static_cast< typename detail::move_or_copy_construct_ref< Cond >::type >(that.m_data.get_cond()),
            that.m_data.deactivate()
        )
    {
    }

    scope_exit& operator= (scope_exit&&) = delete;

    scope_exit(scope_exit const&) = delete;
    scope_exit& operator= (scope_exit const&) = delete;

    /*!
     * \brief If `active() == true`, and invoking the condition function object returns \c true, invokes
     *        the wrapped callable action function object. Destroys the function objects.
     *
     * **Throws:** Nothing, unless invoking a function object throws.
     */
    ~scope_exit()
        noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(
            detail::conjunction<
                detail::is_nothrow_invocable< Func& >,
                detail::is_nothrow_invocable< Cond& >
            >::value
        ))
    {
        if (BOOST_LIKELY(m_data.m_active && m_data.get_cond()()))
            m_data.get_func()();
    }

    /*!
     * \brief Returns \c true if the scope guard is active, otherwise \c false.
     *
     * \note This method does not call the condition function object specified on construction.
     *
     * **Throws:** Nothing.
     */
    bool active() const noexcept
    {
        return m_data.m_active;
    }

    /*!
     * \brief Activates or deactivates the scope guard.
     *
     * **Throws:** Nothing.
     *
     * \param active The active status to set.
     *
     * \post `this->active() == active`
     */
    void set_active(bool active) noexcept
    {
        m_data.m_active = active;
    }
};

#if !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)
template< typename Func >
explicit scope_exit(Func) -> scope_exit< Func >;

template< typename Func >
explicit scope_exit(Func, bool) -> scope_exit< Func >;

template< typename Func, typename Cond >
explicit scope_exit(Func, Cond) -> scope_exit< Func, Cond >;

template< typename Func, typename Cond >
explicit scope_exit(Func, Cond, bool) -> scope_exit< Func, Cond >;
#endif // !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)

/*!
 * \brief Creates a scope guard with a given action function object.
 *
 * **Effects:** Constructs a scope guard as if by calling
 *              `scope_exit< std::decay_t< F > >(std::forward< F >(func), active)`.
 *
 * \param func The callable action function object to invoke on destruction.
 * \param active Indicates whether the scope guard should be active upon construction.
 */
template< typename F >
inline scope_exit< typename std::decay< F >::type > make_scope_exit(F&& func, bool active = true)
    noexcept(std::is_nothrow_constructible<
        scope_exit< typename std::decay< F >::type >,
        F,
        bool
    >::value)
{
    return scope_exit< typename std::decay< F >::type >(static_cast< F&& >(func), active);
}

/*!
 * \brief Creates a conditional scope guard with given callable function objects.
 *
 * **Effects:** Constructs a scope guard as if by calling
 *              `scope_exit< std::decay_t< F >, std::decay_t< C > >(
 *              std::forward< F >(func), std::forward< C >(cond), active)`.
 *
 * \param func The callable action function object to invoke on destruction.
 * \param cond The callable condition function object.
 * \param active Indicates whether the scope guard should be active upon construction.
 */
template< typename F, typename C >
inline
#if !defined(BOOST_SCOPE_DOXYGEN)
typename std::enable_if<
    std::is_constructible<
        scope_exit< typename std::decay< F >::type, typename std::decay< C >::type >,
        F,
        C,
        bool
    >::value,
    scope_exit< typename std::decay< F >::type, typename std::decay< C >::type >
>::type
#else
scope_exit< typename std::decay< F >::type, typename std::decay< C >::type >
#endif
make_scope_exit(F&& func, C&& cond, bool active = true)
    noexcept(std::is_nothrow_constructible<
        scope_exit< typename std::decay< F >::type, typename std::decay< C >::type >,
        F,
        C,
        bool
    >::value)
{
    return scope_exit< typename std::decay< F >::type, typename std::decay< C >::type >(static_cast< F&& >(func), static_cast< C&& >(cond), active);
}

} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_SCOPE_EXIT_HPP_INCLUDED_
