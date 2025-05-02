/*
 *             Copyright Andrey Semashev 2024.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   functor.hpp
 * \author Andrey Semashev
 * \date   2024-01-23
 *
 * This header contains a \c functor implementation. This is a function object
 * that invokes a function that is specified as its template parameter.
 */

#ifndef BOOST_CORE_FUNCTOR_HPP
#define BOOST_CORE_FUNCTOR_HPP

namespace boost::core {

// Block unintended ADL
namespace functor_ns {

//! A function object that invokes a function specified as its template parameter
template< auto Function >
struct functor
{
    template< typename... Args >
    auto operator() (Args&&... args) const noexcept(noexcept(Function(static_cast< Args&& >(args)...))) -> decltype(Function(static_cast< Args&& >(args)...))
    {
        return Function(static_cast< Args&& >(args)...);
    }
};

} // namespace functor_ns

using functor_ns::functor;

} // namespace boost::core

#endif // BOOST_CORE_FUNCTOR_HPP
