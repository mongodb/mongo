#ifndef BOOST_ATOMIC_DETAIL_GCC_ARMV6PLUS_HPP
#define BOOST_ATOMIC_DETAIL_GCC_ARMV6PLUS_HPP

//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  Copyright (c) 2009 Helge Bahmann
//  Copyright (c) 2009 Phil Endecott
//  Copyright (c) 2013 Tim Blechmann
//  ARM Code by Phil Endecott, based on other architectures.

#include <boost/cstdint.hpp>
#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

// From the ARM Architecture Reference Manual for architecture v6:
//
// LDREX{<cond>} <Rd>, [<Rn>]
// <Rd> Specifies the destination register for the memory word addressed by <Rd>
// <Rn> Specifies the register containing the address.
//
// STREX{<cond>} <Rd>, <Rm>, [<Rn>]
// <Rd> Specifies the destination register for the returned status value.
//      0  if the operation updates memory
//      1  if the operation fails to update memory
// <Rm> Specifies the register containing the word to be stored to memory.
// <Rn> Specifies the register containing the address.
// Rd must not be the same register as Rm or Rn.
//
// ARM v7 is like ARM v6 plus:
// There are half-word and byte versions of the LDREX and STREX instructions,
// LDREXH, LDREXB, STREXH and STREXB.
// There are also double-word versions, LDREXD and STREXD.
// (Actually it looks like these are available from version 6k onwards.)
// FIXME these are not yet used; should be mostly a matter of copy-and-paste.
// I think you can supply an immediate offset to the address.
//
// A memory barrier is effected using a "co-processor 15" instruction,
// though a separate assembler mnemonic is available for it in v7.

namespace boost {
namespace atomics {
namespace detail {

// "Thumb 1" is a subset of the ARM instruction set that uses a 16-bit encoding.  It
// doesn't include all instructions and in particular it doesn't include the co-processor
// instruction used for the memory barrier or the load-locked/store-conditional
// instructions.  So, if we're compiling in "Thumb 1" mode, we need to wrap all of our
// asm blocks with code to temporarily change to ARM mode.
//
// You can only change between ARM and Thumb modes when branching using the bx instruction.
// bx takes an address specified in a register.  The least significant bit of the address
// indicates the mode, so 1 is added to indicate that the destination code is Thumb.
// A temporary register is needed for the address and is passed as an argument to these
// macros.  It must be one of the "low" registers accessible to Thumb code, specified
// using the "l" attribute in the asm statement.
//
// Architecture v7 introduces "Thumb 2", which does include (almost?) all of the ARM
// instruction set.  So in v7 we don't need to change to ARM mode; we can write "universal
// assembler" which will assemble to Thumb 2 or ARM code as appropriate.  The only thing
// we need to do to make this "universal" assembler mode work is to insert "IT" instructions
// to annotate the conditional instructions.  These are ignored in other modes (e.g. v6),
// so they can always be present.

#if defined(__thumb__) && !defined(__thumb2__)
#define BOOST_ATOMIC_ARM_ASM_START(TMPREG) "adr " #TMPREG ", 1f\n" "bx " #TMPREG "\n" ".arm\n" ".align 4\n" "1: "
#define BOOST_ATOMIC_ARM_ASM_END(TMPREG)   "adr " #TMPREG ", 1f + 1\n" "bx " #TMPREG "\n" ".thumb\n" ".align 2\n" "1: "
#else
// The tmpreg is wasted in this case, which is non-optimal.
#define BOOST_ATOMIC_ARM_ASM_START(TMPREG)
#define BOOST_ATOMIC_ARM_ASM_END(TMPREG)
#endif

#if defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7S__)
#define BOOST_ATOMIC_ARM_DMB "dmb\n"
#else
#define BOOST_ATOMIC_ARM_DMB "mcr\tp15, 0, r0, c7, c10, 5\n"
#endif

inline void
arm_barrier(void) BOOST_NOEXCEPT
{
    int brtmp;
    __asm__ __volatile__
    (
        BOOST_ATOMIC_ARM_ASM_START(%0)
        BOOST_ATOMIC_ARM_DMB
        BOOST_ATOMIC_ARM_ASM_END(%0)
        : "=&l" (brtmp) :: "memory"
    );
}

inline void
platform_fence_before(memory_order order) BOOST_NOEXCEPT
{
    switch(order)
    {
    case memory_order_release:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        arm_barrier();
    case memory_order_consume:
    default:;
    }
}

inline void
platform_fence_after(memory_order order) BOOST_NOEXCEPT
{
    switch(order)
    {
    case memory_order_acquire:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        arm_barrier();
    default:;
    }
}

inline void
platform_fence_before_store(memory_order order) BOOST_NOEXCEPT
{
    platform_fence_before(order);
}

inline void
platform_fence_after_store(memory_order order) BOOST_NOEXCEPT
{
    if (order == memory_order_seq_cst)
        arm_barrier();
}

inline void
platform_fence_after_load(memory_order order) BOOST_NOEXCEPT
{
    platform_fence_after(order);
}

template<typename T>
inline bool
platform_cmpxchg32(T & expected, T desired, volatile T * ptr) BOOST_NOEXCEPT
{
    int success;
    int tmp;
    __asm__ __volatile__
    (
        BOOST_ATOMIC_ARM_ASM_START(%2)
        "mov     %1, #0\n"        // success = 0
        "ldrex   %0, %3\n"      // expected' = *(&i)
        "teq     %0, %4\n"        // flags = expected'==expected
        "ittt    eq\n"
        "strexeq %2, %5, %3\n"  // if (flags.equal) *(&i) = desired, tmp = !OK
        "teqeq   %2, #0\n"        // if (flags.equal) flags = tmp==0
        "moveq   %1, #1\n"        // if (flags.equal) success = 1
        BOOST_ATOMIC_ARM_ASM_END(%2)
            : "=&r" (expected),  // %0
            "=&r" (success),   // %1
            "=&l" (tmp),       // %2
            "+Q" (*ptr)          // %3
            : "r" (expected),    // %4
            "r" (desired) // %5
            : "cc"
    );
    return success;
}

}
}

#define BOOST_ATOMIC_THREAD_FENCE 2
inline void
atomic_thread_fence(memory_order order)
{
    switch(order)
    {
    case memory_order_acquire:
    case memory_order_release:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        atomics::detail::arm_barrier();
    default:;
    }
}

#define BOOST_ATOMIC_SIGNAL_FENCE 2
inline void
atomic_signal_fence(memory_order)
{
    __asm__ __volatile__ ("" ::: "memory");
}

class atomic_flag
{
private:
    uint32_t v_;

public:
    BOOST_CONSTEXPR atomic_flag(void) BOOST_NOEXCEPT : v_(0) {}

    void
    clear(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        atomics::detail::platform_fence_before_store(order);
        const_cast<volatile uint32_t &>(v_) = 0;
        atomics::detail::platform_fence_after_store(order);
    }

    bool
    test_and_set(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        atomics::detail::platform_fence_before(order);
        uint32_t expected = v_;
        do {
            if (expected == 1)
                break;
        } while (!atomics::detail::platform_cmpxchg32(expected, (uint32_t)1, &v_));
        atomics::detail::platform_fence_after(order);
        return expected;
    }

    BOOST_DELETED_FUNCTION(atomic_flag(const atomic_flag &))
    BOOST_DELETED_FUNCTION(atomic_flag& operator=(const atomic_flag &))
};

#define BOOST_ATOMIC_FLAG_LOCK_FREE 2

}

#undef BOOST_ATOMIC_ARM_ASM_START
#undef BOOST_ATOMIC_ARM_ASM_END

#include <boost/atomic/detail/base.hpp>

#if !defined(BOOST_ATOMIC_FORCE_FALLBACK)

#define BOOST_ATOMIC_CHAR_LOCK_FREE 2
#define BOOST_ATOMIC_CHAR16_T_LOCK_FREE 2
#define BOOST_ATOMIC_CHAR32_T_LOCK_FREE 2
#define BOOST_ATOMIC_WCHAR_T_LOCK_FREE 2
#define BOOST_ATOMIC_SHORT_LOCK_FREE 2
#define BOOST_ATOMIC_INT_LOCK_FREE 2
#define BOOST_ATOMIC_LONG_LOCK_FREE 2
#define BOOST_ATOMIC_LLONG_LOCK_FREE 0
#define BOOST_ATOMIC_POINTER_LOCK_FREE 2
#define BOOST_ATOMIC_BOOL_LOCK_FREE 2

#include <boost/atomic/detail/cas32weak.hpp>

#endif /* !defined(BOOST_ATOMIC_FORCE_FALLBACK) */

#endif
