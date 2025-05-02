#ifndef BOOST_CORE_DETAIL_SP_THREAD_PAUSE_HPP_INCLUDED
#define BOOST_CORE_DETAIL_SP_THREAD_PAUSE_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

// boost/core/detail/sp_thread_pause.hpp
//
// inline void bost::core::sp_thread_pause();
//
//   Emits a "pause" instruction.
//
// Copyright 2008, 2020, 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/config.hpp>

#if defined(__has_builtin)
# if __has_builtin(__builtin_ia32_pause) && !defined(__INTEL_COMPILER)
#  define BOOST_CORE_HAS_BUILTIN_IA32_PAUSE
# endif
#endif

#if defined(BOOST_CORE_HAS_BUILTIN_IA32_PAUSE)

# define BOOST_CORE_SP_PAUSE() __builtin_ia32_pause()

#elif defined(_MSC_VER) && ( defined(_M_IX86) || defined(_M_X64) )

# include <intrin.h>
# define BOOST_CORE_SP_PAUSE() _mm_pause()

#elif defined(_MSC_VER) && ( defined(_M_ARM) || defined(_M_ARM64) )

# include <intrin.h>
# define BOOST_CORE_SP_PAUSE() __yield()

#elif defined(__GNUC__) && ( defined(__i386__) || defined(__x86_64__) )

# define BOOST_CORE_SP_PAUSE() __asm__ __volatile__( "rep; nop" : : : "memory" )

#elif defined(__GNUC__) && ( (defined(__ARM_ARCH) && __ARM_ARCH >= 8) || defined(__ARM_ARCH_8A__) || defined(__aarch64__) )

# define BOOST_CORE_SP_PAUSE() __asm__ __volatile__( "yield" : : : "memory" )

#else

# define BOOST_CORE_SP_PAUSE() ((void)0)

#endif

namespace boost
{
namespace core
{

BOOST_FORCEINLINE void sp_thread_pause() BOOST_NOEXCEPT
{
    BOOST_CORE_SP_PAUSE();
}

} // namespace core
} // namespace boost

#undef BOOST_CORE_SP_PAUSE

#endif // #ifndef BOOST_CORE_DETAIL_SP_THREAD_PAUSE_HPP_INCLUDED
