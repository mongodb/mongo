/*
 *          Copyright Andrey Semashev 2007 - 2013.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   empty_deleter.hpp
 * \author Andrey Semashev
 * \date   22.04.2007
 *
 * This header contains an \c empty_deleter implementation. This is an empty
 * function object that receives a pointer and does nothing with it.
 * Such empty deletion strategy may be convenient, for example, when
 * constructing <tt>shared_ptr</tt>s that point to some object that should not be
 * deleted (i.e. a variable on the stack or some global singleton, like <tt>std::cout</tt>).
 */

#ifndef BOOST_UTILITY_EMPTY_DELETER_HPP_INCLUDED_
#define BOOST_UTILITY_EMPTY_DELETER_HPP_INCLUDED_

#include <boost/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

//! A function object that does nothing and can be used as an empty deleter for \c shared_ptr
struct empty_deleter
{
    //! Function object result type
    typedef void result_type;
    /*!
     * Does nothing
     */
    void operator() (const volatile void*) const BOOST_NOEXCEPT {}
};

} // namespace boost

#endif // BOOST_UTILITY_EMPTY_DELETER_HPP_INCLUDED_
