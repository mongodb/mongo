/* vim: set shiftwidth=4 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* compile-time and runtime tests for whether to use SSE instructions */

#include "SSE.h"

#include "mozilla/Attributes.h"

#ifdef HAVE_CPUID_H
// cpuid.h is available on gcc 4.3 and higher on i386 and x86_64
#  include <cpuid.h>
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_AMD64))
// MSVC 2005 or newer on x86-32 or x86-64
#  include <intrin.h>
#endif

namespace {

// SSE.h has parallel #ifs which declare MOZILLA_SSE_HAVE_CPUID_DETECTION.
// We can't declare these functions in the header file, however, because
// <intrin.h> conflicts with <windows.h> on MSVC 2005, and some files want to
// include both SSE.h and <windows.h>.

#ifdef HAVE_CPUID_H

enum CPUIDRegister { eax = 0, ebx = 1, ecx = 2, edx = 3 };

static bool has_cpuid_bits(unsigned int level, CPUIDRegister reg,
                           unsigned int bits) {
  unsigned int regs[4];
  unsigned int eax, ebx, ecx, edx;
  unsigned max = __get_cpuid_max(level & 0x80000000u, nullptr);
  if (level > max) return false;
  __cpuid_count(level, 0, eax, ebx, ecx, edx);
  regs[0] = eax;
  regs[1] = ebx;
  regs[2] = ecx;
  regs[3] = edx;
  return (regs[reg] & bits) == bits;
}

static bool has_cpuid_bits_ex(unsigned int level, CPUIDRegister reg,
                              unsigned int bits) {
  unsigned int regs[4];
  unsigned int eax, ebx, ecx, edx;
  unsigned max = __get_cpuid_max(level & 0x80000000u, nullptr);
  if (level > max) return false;
  __cpuid_count(level, 1, eax, ebx, ecx, edx);
  regs[0] = eax;
  regs[1] = ebx;
  regs[2] = ecx;
  regs[3] = edx;
  return (regs[reg] & bits) == bits;
}

#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_AMD64))

enum CPUIDRegister { eax = 0, ebx = 1, ecx = 2, edx = 3 };

static bool has_cpuid_bits(unsigned int level, CPUIDRegister reg,
                           unsigned int bits) {
  // Check that the level in question is supported.
  int regs[4];
  __cpuid_ex(regs, level & 0x80000000u, 1);
  if (unsigned(regs[0]) < level) return false;

  // "The __cpuid intrinsic clears the ECX register before calling the cpuid
  // instruction."
  __cpuid_ex(regs, level, 1);
  return (unsigned(regs[reg]) & bits) == bits;
}

#elif (defined(__GNUC__) || defined(__SUNPRO_CC)) && \
    (defined(__i386) || defined(__x86_64__))

enum CPUIDRegister { eax = 0, ebx = 1, ecx = 2, edx = 3 };

#  ifdef __i386
static void moz_cpuid(int CPUInfo[4], int InfoType) {
  asm("xchg %esi, %ebx\n"
      "xor %ecx, %ecx\n"  // ecx is the sub-leaf (we only ever need 0)
      "cpuid\n"
      "movl %eax, (%edi)\n"
      "movl %ebx, 4(%edi)\n"
      "movl %ecx, 8(%edi)\n"
      "movl %edx, 12(%edi)\n"
      "xchg %esi, %ebx\n"
      :
      : "a"(InfoType),  // %eax
        "D"(CPUInfo)    // %edi
      : "%ecx", "%edx", "%esi");
}
static void moz_cpuid_ex(int CPUInfo[4], int InfoType) {
  asm("xchg %esi, %ebx\n"
      "movl 1, %ecx\n"
      "cpuid\n"
      "movl %eax, (%edi)\n"
      "movl %ebx, 4(%edi)\n"
      "movl %ecx, 8(%edi)\n"
      "movl %edx, 12(%edi)\n"
      "xchg %esi, %ebx\n"
      :
      : "a"(InfoType),  // %eax
        "D"(CPUInfo)    // %edi
      : "%ecx", "%edx", "%esi");
}
#  else
static void moz_cpuid(int CPUInfo[4], int InfoType) {
  asm("xchg %rsi, %rbx\n"
      "xor %ecx, %ecx\n"  // ecx is the sub-leaf (we only ever need 0)
      "cpuid\n"
      "movl %eax, (%rdi)\n"
      "movl %ebx, 4(%rdi)\n"
      "movl %ecx, 8(%rdi)\n"
      "movl %edx, 12(%rdi)\n"
      "xchg %rsi, %rbx\n"
      :
      : "a"(InfoType),  // %eax
        "D"(CPUInfo)    // %rdi
      : "%ecx", "%edx", "%rsi");
}
static void moz_cpuid_ex(int CPUInfo[4], int InfoType) {
  asm("xchg %rsi, %rbx\n"
      "movl 1, %ecx\n"
      "cpuid\n"
      "movl %eax, (%rdi)\n"
      "movl %ebx, 4(%rdi)\n"
      "movl %ecx, 8(%rdi)\n"
      "movl %edx, 12(%rdi)\n"
      "xchg %rsi, %rbx\n"
      :
      : "a"(InfoType),  // %eax
        "D"(CPUInfo)    // %rdi
      : "%ecx", "%edx", "%rsi");
}
#  endif

static bool has_cpuid_bits(unsigned int level, CPUIDRegister reg,
                           unsigned int bits) {
  // Check that the level in question is supported.
  volatile int regs[4];
  moz_cpuid((int*)regs, level & 0x80000000u);
  if (unsigned(regs[0]) < level) return false;

  moz_cpuid((int*)regs, level);
  return (unsigned(regs[reg]) & bits) == bits;
}

static bool has_cpuid_bits_ex(unsigned int level, CPUIDRegister reg,
                              unsigned int bits) {
  // Check that the level in question is supported.
  volatile int regs[4];
  moz_cpuid_ex((int*)regs, level & 0x80000000u);
  if (unsigned(regs[0]) < level) return false;

  moz_cpuid_ex((int*)regs, level);
  return (unsigned(regs[reg]) & bits) == bits;
}

#endif  // end CPUID declarations

}  // namespace

namespace mozilla {

namespace sse_private {

#if defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)

#  if !defined(MOZILLA_PRESUME_MMX)
MOZ_RUNINIT bool mmx_enabled = has_cpuid_bits(1u, edx, (1u << 23));
#  endif

#  if !defined(MOZILLA_PRESUME_SSE)
MOZ_RUNINIT bool sse_enabled = has_cpuid_bits(1u, edx, (1u << 25));
#  endif

#  if !defined(MOZILLA_PRESUME_SSE2)
MOZ_RUNINIT bool sse2_enabled = has_cpuid_bits(1u, edx, (1u << 26));
#  endif

#  if !defined(MOZILLA_PRESUME_SSE3)
MOZ_RUNINIT bool sse3_enabled = has_cpuid_bits(1u, ecx, (1u << 0));
#  endif

#  if !defined(MOZILLA_PRESUME_SSSE3)
MOZ_RUNINIT bool ssse3_enabled = has_cpuid_bits(1u, ecx, (1u << 9));
#  endif

#  if !defined(MOZILLA_PRESUME_SSE4A)
MOZ_RUNINIT bool sse4a_enabled = has_cpuid_bits(0x80000001u, ecx, (1u << 6));
#  endif

#  if !defined(MOZILLA_PRESUME_SSE4_1)
MOZ_RUNINIT bool sse4_1_enabled = has_cpuid_bits(1u, ecx, (1u << 19));
#  endif

#  if !defined(MOZILLA_PRESUME_SSE4_2)
MOZ_RUNINIT bool sse4_2_enabled = has_cpuid_bits(1u, ecx, (1u << 20));
#  endif

#  if !defined(MOZILLA_PRESUME_FMA3)
MOZ_RUNINIT bool fma3_enabled = has_cpuid_bits(1u, ecx, (1u << 12));
#  endif

#  if !defined(MOZILLA_PRESUME_AVX) || !defined(MOZILLA_PRESUME_AVX2)
static bool has_avx() {
#    if defined(MOZILLA_PRESUME_AVX)
  return true;
#    else
  const unsigned AVX = 1u << 28;
  const unsigned OSXSAVE = 1u << 27;
  const unsigned XSAVE = 1u << 26;

  const unsigned XMM_STATE = 1u << 1;
  const unsigned YMM_STATE = 1u << 2;
  const unsigned AVX_STATE = XMM_STATE | YMM_STATE;

  return has_cpuid_bits(1u, ecx, AVX | OSXSAVE | XSAVE) &&
         // ensure the OS supports XSAVE of YMM registers
         (xgetbv(0) & AVX_STATE) == AVX_STATE;
#    endif  // MOZILLA_PRESUME_AVX
}
#  endif  // !MOZILLA_PRESUME_AVX || !MOZILLA_PRESUME_AVX2

#  if !defined(MOZILLA_PRESUME_AVX)
MOZ_RUNINIT bool avx_enabled = has_avx();
#  endif

#  if !defined(MOZILLA_PRESUME_AVX2)
MOZ_RUNINIT bool avx2_enabled = has_avx() && has_cpuid_bits(7u, ebx, (1u << 5));
#  endif

#  if !defined(MOZILLA_PRESUME_AVXVNNI)
MOZ_RUNINIT bool avxvnni_enabled = has_cpuid_bits_ex(7u, eax, (1u << 4));
#  endif

#  if !defined(MOZILLA_PRESUME_AES)
MOZ_RUNINIT bool aes_enabled = has_cpuid_bits(1u, ecx, (1u << 25));
#  endif

MOZ_RUNINIT bool has_constant_tsc = has_cpuid_bits(0x80000007u, edx, (1u << 8));

#endif

}  // namespace sse_private

#ifdef HAVE_CPUID_H

uint64_t xgetbv(uint32_t xcr) {
  uint32_t eax, edx;
  __asm__(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(xcr));
  return (uint64_t)(edx) << 32 | eax;
}

#endif

}  // namespace mozilla
