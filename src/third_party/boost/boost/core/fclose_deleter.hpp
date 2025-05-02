/*
 *             Copyright Andrey Semashev 2022.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   fclose_deleter.hpp
 * \author Andrey Semashev
 * \date   21.09.2022
 *
 * This header contains an \c fclose_deleter implementation. This is a deleter
 * function object that invokes <tt>std::fclose</tt> on the passed pointer to
 * a <tt>std::FILE</tt> structure.
 */

#ifndef BOOST_CORE_FCLOSE_DELETER_HPP
#define BOOST_CORE_FCLOSE_DELETER_HPP

#include <cstdio>
#include <boost/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

// Block unintended ADL
namespace fclose_deleter_ns {

//! A function object that closes a file
struct fclose_deleter
{
    //! Function object result type
    typedef void result_type;
    /*!
     * Closes the file handle
     */
    void operator() (std::FILE* p) const BOOST_NOEXCEPT
    {
        if (BOOST_LIKELY(!!p))
            std::fclose(p);
    }
};

} // namespace fclose_deleter_ns

using fclose_deleter_ns::fclose_deleter;

} // namespace boost

#endif // BOOST_CORE_FCLOSE_DELETER_HPP
