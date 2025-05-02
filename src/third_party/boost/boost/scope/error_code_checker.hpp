/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/error_code_checker.hpp
 *
 * This header contains definition of \c error_code_checker type.
 */

#ifndef BOOST_SCOPE_ERROR_CODE_CHECKER_HPP_INCLUDED_
#define BOOST_SCOPE_ERROR_CODE_CHECKER_HPP_INCLUDED_

#include <boost/core/addressof.hpp>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {

/*!
 * \brief A predicate for checking whether an error code indicates error.
 *
 * The predicate captures a reference to an external error code object, which it
 * tests for an error indication when called. The error code object must remain
 * valid for the whole lifetime duration of the predicate.
 *
 * For an error code object `ec`, an expression `!ec` must be valid, never throw exceptions,
 * and return a value contextually convertible to `bool`. If the returned value converts
 * to `false`, then this is taken as an error indication, and the predicate returns `true`.
 * Otherwise, the predicate returns `false`.
 *
 * A few examples of error code types:
 *
 * \li `std::error_code` or `boost::system::error_code`,
 * \li `std::expected`, `boost::outcome_v2::basic_outcome` or `boost::outcome_v2::basic_result`,
 * \li `int`, where the value of 0 indicates no error,
 * \li `bool`, where the value of `false` indicates no error,
 * \li `T*`, where a null pointer indicates no error.
 *
 * \tparam ErrorCode Error code type.
 */
template< typename ErrorCode >
class error_code_checker
{
public:
    //! Predicate result type
    using result_type = bool;

private:
    ErrorCode* m_error_code;

public:
    /*!
     * \brief Constructs the predicate.
     *
     * Upon construction, the predicate saves a reference to the external error code object.
     * The referenced object must remain valid for the whole lifetime duration of the predicate.
     *
     * **Throws:** Nothing.
     */
    explicit error_code_checker(ErrorCode& ec) noexcept :
        m_error_code(boost::addressof(ec))
    {
    }

    /*!
     * \brief Checks if the error code indicates error.
     *
     * **Throws:** Nothing.
     *
     * \returns As if `!!ec`, where `ec` is the error code object passed to the predicate constructor.
     */
    result_type operator()() const noexcept
    {
        return !!(*m_error_code);
    }
};

/*!
 * \brief Creates a predicate for checking whether an exception is being thrown
 *
 * **Throws:** Nothing.
 */
template< typename ErrorCode >
inline error_code_checker< ErrorCode > check_error_code(ErrorCode& ec) noexcept
{
    return error_code_checker< ErrorCode >(ec);
}

} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_ERROR_CODE_CHECKER_HPP_INCLUDED_
