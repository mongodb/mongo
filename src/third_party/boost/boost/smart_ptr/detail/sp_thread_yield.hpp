#ifndef BOOST_SMART_PTR_DETAIL_SP_THREAD_YIELD_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SP_THREAD_YIELD_HPP_INCLUDED

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/core/yield_primitives.hpp>
#include <boost/config/header_deprecated.hpp>

BOOST_HEADER_DEPRECATED( "<boost/core/yield_primitives.hpp>" )

namespace boost
{
namespace detail
{

using boost::core::sp_thread_yield;

} // namespace detail
} // namespace boost

#endif // #ifndef BOOST_SMART_PTR_DETAIL_SP_THREAD_YIELD_HPP_INCLUDED
