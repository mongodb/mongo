#ifndef BOOST_SMART_PTR_DETAIL_DEPRECATED_MACROS_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_DEPRECATED_MACROS_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/config/pragma_message.hpp>

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)

BOOST_PRAGMA_MESSAGE("The macro BOOST_SP_ENABLE_DEBUG_HOOKS has been deprecated in 1.87 and support for it will be removed.")

#endif

#if defined(BOOST_SP_USE_STD_ALLOCATOR)

BOOST_PRAGMA_MESSAGE("The macro BOOST_SP_USE_STD_ALLOCATOR has been deprecated in 1.87 and support for it will be removed.")

#endif

#if defined(BOOST_SP_USE_QUICK_ALLOCATOR)

BOOST_PRAGMA_MESSAGE("The macro BOOST_SP_USE_QUICK_ALLOCATOR has been deprecated in 1.87 and support for it will be removed.")

#endif

#if defined(BOOST_AC_USE_SPINLOCK)

BOOST_PRAGMA_MESSAGE("The macro BOOST_AC_USE_SPINLOCK has been deprecated in 1.87 and support for it will be removed.")

#endif

#if defined(BOOST_AC_USE_PTHREADS)

BOOST_PRAGMA_MESSAGE("The macro BOOST_AC_USE_PTHREADS has been deprecated in 1.87 and support for it will be removed.")

#endif

#if defined(BOOST_SP_USE_SPINLOCK)

BOOST_PRAGMA_MESSAGE("The macro BOOST_SP_USE_SPINLOCK has been deprecated in 1.87 and support for it will be removed.")

#endif

#if defined(BOOST_SP_USE_PTHREADS)

BOOST_PRAGMA_MESSAGE("The macro BOOST_SP_USE_PTHREADS has been deprecated in 1.87 and support for it will be removed.")

#endif

#endif // #ifndef BOOST_SMART_PTR_DETAIL_DEPRECATED_MACROS_HPP_INCLUDED
