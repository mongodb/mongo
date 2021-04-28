/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2020 Andrey Semashev
 */
/*!
 * \file   cpuid.hpp
 *
 * This file contains declaration of \c cpuid function
 */

#ifndef BOOST_ATOMIC_CPUID_HPP_INCLUDED_
#define BOOST_ATOMIC_CPUID_HPP_INCLUDED_

#include <boost/predef/architecture/x86.h>

#if BOOST_ARCH_X86

#if defined(_MSC_VER)
#include <intrin.h> // __cpuid
#endif
#include <boost/cstdint.hpp>
#include <boost/atomic/detail/config.hpp>

#include <boost/atomic/detail/header.hpp>

namespace boost {
namespace atomics {
namespace detail {

//! The function invokes x86 cpuid instruction
inline void cpuid(uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx)
{
#if defined(__GNUC__)
#if (defined(__i386__) || defined(__VXWORKS__)) && (defined(__PIC__) || defined(__PIE__)) && !(defined(__clang__) || (defined(BOOST_GCC) && BOOST_GCC >= 50100))
    // Unless the compiler can do it automatically, we have to backup ebx in 32-bit PIC/PIE code because it is reserved by the ABI.
    // For VxWorks ebx is reserved on 64-bit as well.
#if defined(__x86_64__)
    uint64_t rbx = ebx;
    __asm__ __volatile__
    (
        "xchgq %%rbx, %0\n\t"
        "cpuid\n\t"
        "xchgq %%rbx, %0\n\t"
            : "+DS" (rbx), "+a" (eax), "+c" (ecx), "+d" (edx)
    );
    ebx = static_cast< uint32_t >(rbx);
#else // defined(__x86_64__)
    __asm__ __volatile__
    (
        "xchgl %%ebx, %0\n\t"
        "cpuid\n\t"
        "xchgl %%ebx, %0\n\t"
            : "+DS" (ebx), "+a" (eax), "+c" (ecx), "+d" (edx)
    );
#endif // defined(__x86_64__)
#else
    __asm__ __volatile__
    (
        "cpuid\n\t"
            : "+a" (eax), "+b" (ebx), "+c" (ecx), "+d" (edx)
    );
#endif
#elif defined(_MSC_VER)
    int regs[4] = {};
    __cpuid(regs, eax);
    eax = regs[0];
    ebx = regs[1];
    ecx = regs[2];
    edx = regs[3];
#else
#error "Boost.Atomic: Unsupported compiler, cpuid instruction cannot be generated"
#endif
}

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ARCH_X86

#endif // BOOST_ATOMIC_CPUID_HPP_INCLUDED_
