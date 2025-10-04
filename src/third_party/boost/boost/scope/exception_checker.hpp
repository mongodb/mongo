/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/exception_checker.hpp
 *
 * This header contains definition of \c exception_checker type.
 */

#ifndef BOOST_SCOPE_EXCEPTION_CHECKER_HPP_INCLUDED_
#define BOOST_SCOPE_EXCEPTION_CHECKER_HPP_INCLUDED_

#include <boost/assert.hpp>
#include <boost/scope/detail/config.hpp>
#include <boost/core/uncaught_exceptions.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {

/*!
 * \brief A predicate for checking whether an exception is being thrown.
 *
 * On construction, the predicate captures the current number of uncaught exceptions,
 * which it then compares with the number of uncaught exceptions at the point when it
 * is called. If the number increased then a new exception is detected and the predicate
 * returns \c true.
 *
 * \note This predicate is designed for a specific use case with scope guards created on
 *       the stack. It is incompatible with C++20 coroutines and similar facilities (e.g.
 *       fibers and userspace context switching), where the thread of execution may be
 *       suspended after the predicate captures the number of uncaught exceptions and
 *       then resumed in a different context, where the number of uncaught exceptions
 *       has changed. Similarly, it is incompatible with usage patterns where the predicate
 *       is cached after construction and is invoked after the thread has left the scope
 *       where the predicate was constructed (e.g. when the predicate is stored as a class
 *       data member or a namespace-scope variable).
 */
class exception_checker
{
public:
    //! Predicate result type
    using result_type = bool;

private:
    unsigned int m_uncaught_count;

public:
    /*!
     * \brief Constructs the predicate.
     *
     * Upon construction, the predicate saves the current number of uncaught exceptions.
     * This information will be used when calling the predicate to detect if a new
     * exception is being thrown.
     *
     * **Throws:** Nothing.
     */
    exception_checker() noexcept :
        m_uncaught_count(boost::core::uncaught_exceptions())
    {
    }

    /*!
     * \brief Checks if an exception is being thrown.
     *
     * **Throws:** Nothing.
     *
     * \returns \c true if the number of uncaught exceptions at the point of call is
     *          greater than that at the point of construction of the predicate,
     *          otherwise \c false.
     */
    result_type operator()() const noexcept
    {
        const unsigned int uncaught_count = boost::core::uncaught_exceptions();
        // If this assertion fails, the predicate is likely being used in an unsupported
        // way, where it is called in a different scope or thread context from where
        // it was constructed.
        BOOST_ASSERT((uncaught_count - m_uncaught_count) <= 1u);
        return uncaught_count > m_uncaught_count;
    }
};

/*!
 * \brief Creates a predicate for checking whether an exception is being thrown
 *
 * **Throws:** Nothing.
 */
inline exception_checker check_exception() noexcept
{
    return exception_checker();
}

} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_EXCEPTION_CHECKER_HPP_INCLUDED_
