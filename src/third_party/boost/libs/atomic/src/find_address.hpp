/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2020 Andrey Semashev
 */
/*!
 * \file   find_address.hpp
 *
 * This file contains declaration of \c find_address algorithm
 */

#ifndef BOOST_ATOMIC_FIND_ADDRESS_HPP_INCLUDED_
#define BOOST_ATOMIC_FIND_ADDRESS_HPP_INCLUDED_

#include <cstddef>
#include <boost/predef/architecture/x86.h>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/int_sizes.hpp>
#include <boost/atomic/detail/header.hpp>

namespace boost {
namespace atomics {
namespace detail {

//! \c find_address signature
typedef std::size_t (find_address_t)(const volatile void* addr, const volatile void* const* addrs, std::size_t size);

extern find_address_t find_address_generic;

#if BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 || BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4)
extern find_address_t find_address_sse2;
#if BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8
extern find_address_t find_address_sse41;
#endif // BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8
#endif // BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 || BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4)

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ATOMIC_FIND_ADDRESS_HPP_INCLUDED_
