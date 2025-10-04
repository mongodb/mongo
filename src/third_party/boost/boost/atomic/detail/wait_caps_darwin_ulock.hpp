/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2021 Andrey Semashev
 */
/*!
 * \file   atomic/detail/wait_caps_darwin_ulock.hpp
 *
 * This header defines waiting/notifying operations capabilities macros.
 */

#ifndef BOOST_ATOMIC_DETAIL_WAIT_CAPS_DARWIN_ULOCK_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_WAIT_CAPS_DARWIN_ULOCK_HPP_INCLUDED_

#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/capabilities.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#define BOOST_ATOMIC_HAS_NATIVE_INT32_WAIT_NOTIFY BOOST_ATOMIC_INT32_LOCK_FREE

// Darwin 19+ (Mac OS 10.15+, iOS 13.0+, tvOS 13.0+, watchOS 6.0+) adds support for 64-bit
// and inter-process ulock operations.
// https://shift.click/blog/futex-like-apis/#darwin-macos-ios-tvos-watchos-and-more
// https://github.com/thomcc/ulock-sys/blob/2597e63cc5372459a903c292a3919d385a3e3789/src/lib.rs
#if (defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 101500) || \
    (defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ >= 130000) || \
    (defined(__ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__ >= 130000) || \
    (defined(__ENVIRONMENT_WATCH_OS_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_WATCH_OS_VERSION_MIN_REQUIRED__ >= 60000)
#define BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK64
#define BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED
#endif

// Darwin 20+ (Mac OS 11.0+, iOS 14.0+, tvOS 14.0+, watchOS 7.0+) introduces __ulock_wait2, which accepts
// the timeout in nanoseconds. __ulock_wait is a wrapper on top of __ulock_wait2.
#if (defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 110000) || \
    (defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ >= 140000) || \
    (defined(__ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__ >= 140000) || \
    (defined(__ENVIRONMENT_WATCH_OS_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_WATCH_OS_VERSION_MIN_REQUIRED__ >= 70000)
#define BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_WAIT2
#endif

#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
#define BOOST_ATOMIC_HAS_NATIVE_INT32_IPC_WAIT_NOTIFY BOOST_ATOMIC_INT32_LOCK_FREE
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)

#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK64)
#define BOOST_ATOMIC_HAS_NATIVE_INT64_WAIT_NOTIFY BOOST_ATOMIC_INT64_LOCK_FREE
#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
#define BOOST_ATOMIC_HAS_NATIVE_INT64_IPC_WAIT_NOTIFY BOOST_ATOMIC_INT64_LOCK_FREE
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK64)

#endif // BOOST_ATOMIC_DETAIL_WAIT_CAPS_DARWIN_ULOCK_HPP_INCLUDED_
