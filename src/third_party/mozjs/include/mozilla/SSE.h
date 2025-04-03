/* vim: set shiftwidth=2 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* compile-time and runtime tests for whether to use SSE instructions */

#ifndef mozilla_SSE_h_
#define mozilla_SSE_h_

// for definition of MFBT_DATA
#include "mozilla/Types.h"

/**
 * The public interface of this header consists of a set of macros and
 * functions for Intel CPU features.
 *
 * DETECTING ISA EXTENSIONS
 * ========================
 *
 * This header provides the following functions for determining whether the
 * current CPU supports a particular instruction set extension:
 *
 *    mozilla::supports_mmx
 *    mozilla::supports_sse
 *    mozilla::supports_sse2
 *    mozilla::supports_sse3
 *    mozilla::supports_ssse3
 *    mozilla::supports_sse4a
 *    mozilla::supports_sse4_1
 *    mozilla::supports_sse4_2
 *    mozilla::supports_avx
 *    mozilla::supports_avx2
 *    mozilla::supports_aes
 *    mozilla::has_constant_tsc
 *
 * If you're writing code using inline assembly, you should guard it with a
 * call to one of these functions.  For instance:
 *
 *   if (mozilla::supports_sse2()) {
 *     asm(" ... ");
 *   }
 *   else {
 *     ...
 *   }
 *
 * Note that these functions depend on cpuid intrinsics only available in gcc
 * 4.3 or later and MSVC 8.0 (Visual C++ 2005) or later, so they return false
 * in older compilers.  (This could be fixed by replacing the code with inline
 * assembly.)
 *
 *
 * USING INTRINSICS
 * ================
 *
 * This header also provides support for coding using CPU intrinsics.
 *
 * For each mozilla::supports_abc function, we define a MOZILLA_MAY_SUPPORT_ABC
 * macro which indicates that the target/compiler combination we're using is
 * compatible with the ABC extension.  For instance, x86_64 with MSVC 2003 is
 * compatible with SSE2 but not SSE3, since although there exist x86_64 CPUs
 * with SSE3 support, MSVC 2003 only supports through SSE2.
 *
 * Until gcc fixes #pragma target [1] [2] or our x86 builds require SSE2,
 * you'll need to separate code using intrinsics into a file separate from your
 * regular code.  Here's the recommended pattern:
 *
 *  #ifdef MOZILLA_MAY_SUPPORT_ABC
 *    namespace mozilla {
 *      namespace ABC {
 *        void foo();
 *      }
 *    }
 *  #endif
 *
 *  void foo() {
 *    #ifdef MOZILLA_MAY_SUPPORT_ABC
 *      if (mozilla::supports_abc()) {
 *        mozilla::ABC::foo(); // in a separate file
 *        return;
 *      }
 *    #endif
 *
 *    foo_unvectorized();
 *  }
 *
 * You'll need to define mozilla::ABC::foo() in a separate file and add the
 * -mabc flag when using gcc.
 *
 * [1] http://gcc.gnu.org/bugzilla/show_bug.cgi?id=39787 and
 * [2] http://gcc.gnu.org/bugzilla/show_bug.cgi?id=41201 being fixed.
 *
 */

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))

#  ifdef __MMX__
// It's ok to use MMX instructions based on the -march option (or
// the default for x86_64 or for Intel Mac).
#    define MOZILLA_PRESUME_MMX 1
#  endif
#  ifdef __SSE__
// It's ok to use SSE instructions based on the -march option (or
// the default for x86_64 or for Intel Mac).
#    define MOZILLA_PRESUME_SSE 1
#  endif
#  ifdef __SSE2__
// It's ok to use SSE2 instructions based on the -march option (or
// the default for x86_64 or for Intel Mac).
#    define MOZILLA_PRESUME_SSE2 1
#  endif
#  ifdef __SSE3__
// It's ok to use SSE3 instructions based on the -march option (or the
// default for Intel Mac).
#    define MOZILLA_PRESUME_SSE3 1
#  endif
#  ifdef __SSSE3__
// It's ok to use SSSE3 instructions based on the -march option.
#    define MOZILLA_PRESUME_SSSE3 1
#  endif
#  ifdef __SSE4A__
// It's ok to use SSE4A instructions based on the -march option.
#    define MOZILLA_PRESUME_SSE4A 1
#  endif
#  ifdef __SSE4_1__
// It's ok to use SSE4.1 instructions based on the -march option.
#    define MOZILLA_PRESUME_SSE4_1 1
#  endif
#  ifdef __SSE4_2__
// It's ok to use SSE4.2 instructions based on the -march option.
#    define MOZILLA_PRESUME_SSE4_2 1
#  endif
#  ifdef __AVX__
// It's ok to use AVX instructions based on the -march option.
#    define MOZILLA_PRESUME_AVX 1
#  endif
#  ifdef __AVX2__
// It's ok to use AVX instructions based on the -march option.
#    define MOZILLA_PRESUME_AVX2 1
#  endif
#  ifdef __AES__
// It's ok to use AES instructions based on the -march option.
#    define MOZILLA_PRESUME_AES 1
#  endif

#  ifdef HAVE_CPUID_H
#    define MOZILLA_SSE_HAVE_CPUID_DETECTION
#  endif

#elif defined(__SUNPRO_CC) && (defined(__i386) || defined(__x86_64__))
// Sun Studio on x86 or amd64

#  define MOZILLA_SSE_HAVE_CPUID_DETECTION

#  if defined(__x86_64__)
// MMX is always available on AMD64.
#    define MOZILLA_PRESUME_MMX
// SSE is always available on AMD64.
#    define MOZILLA_PRESUME_SSE
// SSE2 is always available on AMD64.
#    define MOZILLA_PRESUME_SSE2
#  endif

#endif

namespace mozilla {

namespace sse_private {
#if defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  if !defined(MOZILLA_PRESUME_MMX)
extern bool MFBT_DATA mmx_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_SSE)
extern bool MFBT_DATA sse_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_SSE2)
extern bool MFBT_DATA sse2_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_SSE3)
extern bool MFBT_DATA sse3_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_SSSE3)
extern bool MFBT_DATA ssse3_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_SSE4A)
extern bool MFBT_DATA sse4a_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_SSE4_1)
extern bool MFBT_DATA sse4_1_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_SSE4_2)
extern bool MFBT_DATA sse4_2_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_FMA3)
extern bool MFBT_DATA fma3_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_AVX)
extern bool MFBT_DATA avx_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_AVX2)
extern bool MFBT_DATA avx2_enabled;
#  endif
#  if !defined(MOZILLA_PRESUME_AES)
extern bool MFBT_DATA aes_enabled;
#  endif
extern bool MFBT_DATA has_constant_tsc;

#endif
}  // namespace sse_private

#ifdef HAVE_CPUID_H
MOZ_EXPORT uint64_t xgetbv(uint32_t xcr);
#endif

#if defined(MOZILLA_PRESUME_MMX)
#  define MOZILLA_MAY_SUPPORT_MMX 1
inline bool supports_mmx() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  if !(defined(_MSC_VER) && defined(_M_AMD64))
// Define MOZILLA_MAY_SUPPORT_MMX only if we're not on MSVC for
// AMD64, since that compiler doesn't support MMX.
#    define MOZILLA_MAY_SUPPORT_MMX 1
#  endif
inline bool supports_mmx() { return sse_private::mmx_enabled; }
#else
inline bool supports_mmx() { return false; }
#endif

#if defined(MOZILLA_PRESUME_SSE)
#  define MOZILLA_MAY_SUPPORT_SSE 1
inline bool supports_sse() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_SSE 1
inline bool supports_sse() { return sse_private::sse_enabled; }
#else
inline bool supports_sse() { return false; }
#endif

#if defined(MOZILLA_PRESUME_SSE2)
#  define MOZILLA_MAY_SUPPORT_SSE2 1
inline bool supports_sse2() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_SSE2 1
inline bool supports_sse2() { return sse_private::sse2_enabled; }
#else
inline bool supports_sse2() { return false; }
#endif

#if defined(MOZILLA_PRESUME_SSE3)
#  define MOZILLA_MAY_SUPPORT_SSE3 1
inline bool supports_sse3() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_SSE3 1
inline bool supports_sse3() { return sse_private::sse3_enabled; }
#else
inline bool supports_sse3() { return false; }
#endif

#if defined(MOZILLA_PRESUME_SSSE3)
#  define MOZILLA_MAY_SUPPORT_SSSE3 1
inline bool supports_ssse3() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_SSSE3 1
inline bool supports_ssse3() { return sse_private::ssse3_enabled; }
#else
inline bool supports_ssse3() { return false; }
#endif

#if defined(MOZILLA_PRESUME_SSE4A)
#  define MOZILLA_MAY_SUPPORT_SSE4A 1
inline bool supports_sse4a() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_SSE4A 1
inline bool supports_sse4a() { return sse_private::sse4a_enabled; }
#else
inline bool supports_sse4a() { return false; }
#endif

#if defined(MOZILLA_PRESUME_SSE4_1)
#  define MOZILLA_MAY_SUPPORT_SSE4_1 1
inline bool supports_sse4_1() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_SSE4_1 1
inline bool supports_sse4_1() { return sse_private::sse4_1_enabled; }
#else
inline bool supports_sse4_1() { return false; }
#endif

#if defined(MOZILLA_PRESUME_SSE4_2)
#  define MOZILLA_MAY_SUPPORT_SSE4_2 1
inline bool supports_sse4_2() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_SSE4_2 1
inline bool supports_sse4_2() { return sse_private::sse4_2_enabled; }
#else
inline bool supports_sse4_2() { return false; }
#endif

#if defined(MOZILLA_PRESUME_FMA3)
#  define MOZILLA_MAY_SUPPORT_FMA3 1
inline bool supports_fma3() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_FMA3 1
inline bool supports_fma3() { return sse_private::fma3_enabled; }
#else
inline bool supports_fma3() { return false; }
#endif

#if defined(MOZILLA_PRESUME_AVX)
#  define MOZILLA_MAY_SUPPORT_AVX 1
inline bool supports_avx() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_AVX 1
inline bool supports_avx() { return sse_private::avx_enabled; }
#else
inline bool supports_avx() { return false; }
#endif

#if defined(MOZILLA_PRESUME_AVX2)
#  define MOZILLA_MAY_SUPPORT_AVX2 1
inline bool supports_avx2() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_AVX2 1
inline bool supports_avx2() { return sse_private::avx2_enabled; }
#else
inline bool supports_avx2() { return false; }
#endif

#if defined(MOZILLA_PRESUME_AES)
#  define MOZILLA_MAY_SUPPORT_AES 1
inline bool supports_aes() { return true; }
#elif defined(MOZILLA_SSE_HAVE_CPUID_DETECTION)
#  define MOZILLA_MAY_SUPPORT_AES 1
inline bool supports_aes() { return sse_private::aes_enabled; }
#else
inline bool supports_aes() { return false; }
#endif

#ifdef MOZILLA_SSE_HAVE_CPUID_DETECTION
inline bool has_constant_tsc() { return sse_private::has_constant_tsc; }
#else
inline bool has_constant_tsc() { return false; }
#endif

}  // namespace mozilla

#endif /* !defined(mozilla_SSE_h_) */
