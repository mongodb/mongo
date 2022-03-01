/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementations of runtime and static assertion macros for C and C++. */

#ifndef mozilla_Assertions_h
#define mozilla_Assertions_h

#if (defined(MOZ_HAS_MOZGLUE) || defined(MOZILLA_INTERNAL_API)) && \
    !defined(__wasi__)
#  define MOZ_DUMP_ASSERTION_STACK
#endif

#include "mozilla/Attributes.h"
#include "mozilla/Compiler.h"
#include "mozilla/Likely.h"
#include "mozilla/MacroArgs.h"
#include "mozilla/StaticAnalysisFunctions.h"
#include "mozilla/Types.h"
#ifdef MOZ_DUMP_ASSERTION_STACK
#  include "mozilla/StackWalk.h"
#endif

/*
 * The crash reason set by MOZ_CRASH_ANNOTATE is consumed by the crash reporter
 * if present. It is declared here (and defined in Assertions.cpp) to make it
 * available to all code, even libraries that don't link with the crash reporter
 * directly.
 */
MOZ_BEGIN_EXTERN_C
extern MFBT_DATA const char* gMozCrashReason;
MOZ_END_EXTERN_C

#if defined(MOZ_HAS_MOZGLUE) || defined(MOZILLA_INTERNAL_API)
static inline void AnnotateMozCrashReason(const char* reason) {
  gMozCrashReason = reason;
}
#  define MOZ_CRASH_ANNOTATE(...) AnnotateMozCrashReason(__VA_ARGS__)
#else
#  define MOZ_CRASH_ANNOTATE(...) \
    do { /* nothing */            \
    } while (false)
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _MSC_VER
/*
 * TerminateProcess and GetCurrentProcess are defined in <winbase.h>, which
 * further depends on <windef.h>.  We hardcode these few definitions manually
 * because those headers clutter the global namespace with a significant
 * number of undesired macros and symbols.
 */
MOZ_BEGIN_EXTERN_C
__declspec(dllimport) int __stdcall TerminateProcess(void* hProcess,
                                                     unsigned int uExitCode);
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
MOZ_END_EXTERN_C
#elif defined(__wasi__)
/*
 * On Wasm/WASI platforms, we just call __builtin_trap().
 */
#else
#  include <signal.h>
#endif
#ifdef ANDROID
#  include <android/log.h>
#endif

MOZ_BEGIN_EXTERN_C

#if defined(ANDROID) && defined(MOZ_DUMP_ASSERTION_STACK)
MOZ_MAYBE_UNUSED static void MOZ_ReportAssertionFailurePrintFrame(
    const char* aBuf) {
  __android_log_print(ANDROID_LOG_FATAL, "MOZ_Assert", "%s\n", aBuf);
}
#endif

/*
 * Prints |aStr| as an assertion failure (using aFilename and aLine as the
 * location of the assertion) to the standard debug-output channel.
 *
 * Usually you should use MOZ_ASSERT or MOZ_CRASH instead of this method.  This
 * method is primarily for internal use in this header, and only secondarily
 * for use in implementing release-build assertions.
 */
MOZ_MAYBE_UNUSED static MOZ_COLD MOZ_NEVER_INLINE void
MOZ_ReportAssertionFailure(const char* aStr, const char* aFilename,
                           int aLine) MOZ_PRETEND_NORETURN_FOR_STATIC_ANALYSIS {
#ifdef ANDROID
  __android_log_print(ANDROID_LOG_FATAL, "MOZ_Assert",
                      "Assertion failure: %s, at %s:%d\n", aStr, aFilename,
                      aLine);
#  if defined(MOZ_DUMP_ASSERTION_STACK)
  MozWalkTheStackWithWriter(MOZ_ReportAssertionFailurePrintFrame, CallerPC(),
                            /* aMaxFrames */ 0);
#  endif
#else
  fprintf(stderr, "Assertion failure: %s, at %s:%d\n", aStr, aFilename, aLine);
#  if defined(MOZ_DUMP_ASSERTION_STACK)
  MozWalkTheStack(stderr, CallerPC(), /* aMaxFrames */ 0);
#  endif
  fflush(stderr);
#endif
}

MOZ_MAYBE_UNUSED static MOZ_COLD MOZ_NEVER_INLINE void MOZ_ReportCrash(
    const char* aStr, const char* aFilename,
    int aLine) MOZ_PRETEND_NORETURN_FOR_STATIC_ANALYSIS {
#ifdef ANDROID
  __android_log_print(ANDROID_LOG_FATAL, "MOZ_CRASH",
                      "Hit MOZ_CRASH(%s) at %s:%d\n", aStr, aFilename, aLine);
#else
  fprintf(stderr, "Hit MOZ_CRASH(%s) at %s:%d\n", aStr, aFilename, aLine);
#  if defined(MOZ_DUMP_ASSERTION_STACK)
  MozWalkTheStack(stderr, CallerPC(), /* aMaxFrames */ 0);
#  endif
  fflush(stderr);
#endif
}

/**
 * MOZ_REALLY_CRASH is used in the implementation of MOZ_CRASH().  You should
 * call MOZ_CRASH instead.
 */
#if defined(_MSC_VER)
/*
 * On MSVC use the __debugbreak compiler intrinsic, which produces an inline
 * (not nested in a system function) breakpoint.  This distinctively invokes
 * Breakpad without requiring system library symbols on all stack-processing
 * machines, as a nested breakpoint would require.
 *
 * We use __LINE__ to prevent the compiler from folding multiple crash sites
 * together, which would make crash reports hard to understand.
 *
 * We use TerminateProcess with the exit code aborting would generate
 * because we don't want to invoke atexit handlers, destructors, library
 * unload handlers, and so on when our process might be in a compromised
 * state.
 *
 * We don't use abort() because it'd cause Windows to annoyingly pop up the
 * process error dialog multiple times.  See bug 345118 and bug 426163.
 *
 * (Technically these are Windows requirements, not MSVC requirements.  But
 * practically you need MSVC for debugging, and we only ship builds created
 * by MSVC, so doing it this way reduces complexity.)
 */

MOZ_MAYBE_UNUSED static MOZ_COLD MOZ_NORETURN MOZ_NEVER_INLINE void
MOZ_NoReturn(int aLine) {
  *((volatile int*)NULL) = aLine;
  TerminateProcess(GetCurrentProcess(), 3);
}

#  define MOZ_REALLY_CRASH(line) \
    do {                         \
      __debugbreak();            \
      MOZ_NoReturn(line);        \
    } while (false)

#elif __wasi__

#  define MOZ_REALLY_CRASH(line) __builtin_trap()

#else

/*
 * MOZ_CRASH_WRITE_ADDR is the address to be used when performing a forced
 * crash. NULL is preferred however if for some reason NULL cannot be used
 * this makes choosing another value possible.
 *
 * In the case of UBSan certain checks, bounds specifically, cause the compiler
 * to emit the 'ud2' instruction when storing to 0x0. This causes forced
 * crashes to manifest as ILL (at an arbitrary address) instead of the expected
 * SEGV at 0x0.
 */
#  ifdef MOZ_UBSAN
#    define MOZ_CRASH_WRITE_ADDR 0x1
#  else
#    define MOZ_CRASH_WRITE_ADDR NULL
#  endif

#  ifdef __cplusplus
#    define MOZ_REALLY_CRASH(line)                                  \
      do {                                                          \
        *((volatile int*)MOZ_CRASH_WRITE_ADDR) = line; /* NOLINT */ \
        ::abort();                                                  \
      } while (false)
#  else
#    define MOZ_REALLY_CRASH(line)                                  \
      do {                                                          \
        *((volatile int*)MOZ_CRASH_WRITE_ADDR) = line; /* NOLINT */ \
        abort();                                                    \
      } while (false)
#  endif
#endif

/*
 * MOZ_CRASH([explanation-string]) crashes the program, plain and simple, in a
 * Breakpad-compatible way, in both debug and release builds.
 *
 * MOZ_CRASH is a good solution for "handling" failure cases when you're
 * unwilling or unable to handle them more cleanly -- for OOM, for likely memory
 * corruption, and so on.  It's also a good solution if you need safe behavior
 * in release builds as well as debug builds.  But if the failure is one that
 * should be debugged and fixed, MOZ_ASSERT is generally preferable.
 *
 * The optional explanation-string, if provided, must be a string literal
 * explaining why we're crashing.  This argument is intended for use with
 * MOZ_CRASH() calls whose rationale is non-obvious; don't use it if it's
 * obvious why we're crashing.
 *
 * If we're a DEBUG build and we crash at a MOZ_CRASH which provides an
 * explanation-string, we print the string to stderr.  Otherwise, we don't
 * print anything; this is because we want MOZ_CRASH to be 100% safe in release
 * builds, and it's hard to print to stderr safely when memory might have been
 * corrupted.
 */
#ifndef DEBUG
#  define MOZ_CRASH(...)                                \
    do {                                                \
      MOZ_CRASH_ANNOTATE("MOZ_CRASH(" __VA_ARGS__ ")"); \
      MOZ_REALLY_CRASH(__LINE__);                       \
    } while (false)
#else
#  define MOZ_CRASH(...)                                   \
    do {                                                   \
      MOZ_ReportCrash("" __VA_ARGS__, __FILE__, __LINE__); \
      MOZ_CRASH_ANNOTATE("MOZ_CRASH(" __VA_ARGS__ ")");    \
      MOZ_REALLY_CRASH(__LINE__);                          \
    } while (false)
#endif

/*
 * MOZ_CRASH_UNSAFE(explanation-string) can be used if the explanation string
 * cannot be a string literal (but no other processing needs to be done on it).
 * A regular MOZ_CRASH() is preferred wherever possible, as passing arbitrary
 * strings from a potentially compromised process is not without risk. If the
 * string being passed is the result of a printf-style function, consider using
 * MOZ_CRASH_UNSAFE_PRINTF instead.
 *
 * @note This macro causes data collection because crash strings are annotated
 * to crash-stats and are publicly visible. Firefox data stewards must do data
 * review on usages of this macro.
 */
static MOZ_ALWAYS_INLINE_EVEN_DEBUG MOZ_COLD MOZ_NORETURN void MOZ_Crash(
    const char* aFilename, int aLine, const char* aReason) {
#ifdef DEBUG
  MOZ_ReportCrash(aReason, aFilename, aLine);
#endif
  MOZ_CRASH_ANNOTATE(aReason);
  MOZ_REALLY_CRASH(aLine);
}
#define MOZ_CRASH_UNSAFE(reason) MOZ_Crash(__FILE__, __LINE__, reason)

static const size_t sPrintfMaxArgs = 4;
static const size_t sPrintfCrashReasonSize = 1024;

MFBT_API MOZ_COLD MOZ_NEVER_INLINE MOZ_FORMAT_PRINTF(1, 2) const
    char* MOZ_CrashPrintf(const char* aFormat, ...);

/*
 * MOZ_CRASH_UNSAFE_PRINTF(format, arg1 [, args]) can be used when more
 * information is desired than a string literal can supply. The caller provides
 * a printf-style format string, which must be a string literal and between
 * 1 and 4 additional arguments. A regular MOZ_CRASH() is preferred wherever
 * possible, as passing arbitrary strings to printf from a potentially
 * compromised process is not without risk.
 *
 * @note This macro causes data collection because crash strings are annotated
 * to crash-stats and are publicly visible. Firefox data stewards must do data
 * review on usages of this macro.
 */
#define MOZ_CRASH_UNSAFE_PRINTF(format, ...)                                \
  do {                                                                      \
    static_assert(MOZ_ARG_COUNT(__VA_ARGS__) > 0,                           \
                  "Did you forget arguments to MOZ_CRASH_UNSAFE_PRINTF? "   \
                  "Or maybe you want MOZ_CRASH instead?");                  \
    static_assert(MOZ_ARG_COUNT(__VA_ARGS__) <= sPrintfMaxArgs,             \
                  "Only up to 4 additional arguments are allowed!");        \
    static_assert(sizeof(format) <= sPrintfCrashReasonSize,                 \
                  "The supplied format string is too long!");               \
    MOZ_Crash(__FILE__, __LINE__, MOZ_CrashPrintf("" format, __VA_ARGS__)); \
  } while (false)

MOZ_END_EXTERN_C

/*
 * MOZ_ASSERT(expr [, explanation-string]) asserts that |expr| must be truthy in
 * debug builds.  If it is, execution continues.  Otherwise, an error message
 * including the expression and the explanation-string (if provided) is printed,
 * an attempt is made to invoke any existing debugger, and execution halts.
 * MOZ_ASSERT is fatal: no recovery is possible.  Do not assert a condition
 * which can correctly be falsy.
 *
 * The optional explanation-string, if provided, must be a string literal
 * explaining the assertion.  It is intended for use with assertions whose
 * correctness or rationale is non-obvious, and for assertions where the "real"
 * condition being tested is best described prosaically.  Don't provide an
 * explanation if it's not actually helpful.
 *
 *   // No explanation needed: pointer arguments often must not be NULL.
 *   MOZ_ASSERT(arg);
 *
 *   // An explanation can be helpful to explain exactly how we know an
 *   // assertion is valid.
 *   MOZ_ASSERT(state == WAITING_FOR_RESPONSE,
 *              "given that <thingA> and <thingB>, we must have...");
 *
 *   // Or it might disambiguate multiple identical (save for their location)
 *   // assertions of the same expression.
 *   MOZ_ASSERT(getSlot(PRIMITIVE_THIS_SLOT).isUndefined(),
 *              "we already set [[PrimitiveThis]] for this Boolean object");
 *   MOZ_ASSERT(getSlot(PRIMITIVE_THIS_SLOT).isUndefined(),
 *              "we already set [[PrimitiveThis]] for this String object");
 *
 * MOZ_ASSERT has no effect in non-debug builds.  It is designed to catch bugs
 * *only* during debugging, not "in the field". If you want the latter, use
 * MOZ_RELEASE_ASSERT, which applies to non-debug builds as well.
 *
 * MOZ_DIAGNOSTIC_ASSERT works like MOZ_RELEASE_ASSERT in Nightly/Aurora and
 * MOZ_ASSERT in Beta/Release - use this when a condition is potentially rare
 * enough to require real user testing to hit, but is not security-sensitive.
 * This can cause user pain, so use it sparingly. If a MOZ_DIAGNOSTIC_ASSERT
 * is firing, it should promptly be converted to a MOZ_ASSERT while the failure
 * is being investigated, rather than letting users suffer.
 *
 * MOZ_DIAGNOSTIC_ASSERT_ENABLED is defined when MOZ_DIAGNOSTIC_ASSERT is like
 * MOZ_RELEASE_ASSERT rather than MOZ_ASSERT.
 */

/*
 * Implement MOZ_VALIDATE_ASSERT_CONDITION_TYPE, which is used to guard against
 * accidentally passing something unintended in lieu of an assertion condition.
 */

#ifdef __cplusplus
#  include <type_traits>
namespace mozilla {
namespace detail {

template <typename T>
struct AssertionConditionType {
  using ValueT = std::remove_reference_t<T>;
  static_assert(!std::is_array_v<ValueT>,
                "Expected boolean assertion condition, got an array or a "
                "string!");
  static_assert(!std::is_function_v<ValueT>,
                "Expected boolean assertion condition, got a function! Did "
                "you intend to call that function?");
  static_assert(!std::is_floating_point_v<ValueT>,
                "It's often a bad idea to assert that a floating-point number "
                "is nonzero, because such assertions tend to intermittently "
                "fail. Shouldn't your code gracefully handle this case instead "
                "of asserting? Anyway, if you really want to do that, write an "
                "explicit boolean condition, like !!x or x!=0.");

  static const bool isValid = true;
};

}  // namespace detail
}  // namespace mozilla
#  define MOZ_VALIDATE_ASSERT_CONDITION_TYPE(x)                        \
    static_assert(                                                     \
        mozilla::detail::AssertionConditionType<decltype(x)>::isValid, \
        "invalid assertion condition")
#else
#  define MOZ_VALIDATE_ASSERT_CONDITION_TYPE(x)
#endif

#if defined(DEBUG) || defined(MOZ_ASAN)
#  define MOZ_REPORT_ASSERTION_FAILURE(...) \
    MOZ_ReportAssertionFailure(__VA_ARGS__)
#else
#  define MOZ_REPORT_ASSERTION_FAILURE(...) \
    do { /* nothing */                      \
    } while (false)
#endif

/* First the single-argument form. */
#define MOZ_ASSERT_HELPER1(kind, expr)                         \
  do {                                                         \
    MOZ_VALIDATE_ASSERT_CONDITION_TYPE(expr);                  \
    if (MOZ_UNLIKELY(!MOZ_CHECK_ASSERT_ASSIGNMENT(expr))) {    \
      MOZ_REPORT_ASSERTION_FAILURE(#expr, __FILE__, __LINE__); \
      MOZ_CRASH_ANNOTATE(kind "(" #expr ")");                  \
      MOZ_REALLY_CRASH(__LINE__);                              \
    }                                                          \
  } while (false)
/* Now the two-argument form. */
#define MOZ_ASSERT_HELPER2(kind, expr, explain)                      \
  do {                                                               \
    MOZ_VALIDATE_ASSERT_CONDITION_TYPE(expr);                        \
    if (MOZ_UNLIKELY(!MOZ_CHECK_ASSERT_ASSIGNMENT(expr))) {          \
      MOZ_REPORT_ASSERTION_FAILURE(#expr " (" explain ")", __FILE__, \
                                   __LINE__);                        \
      MOZ_CRASH_ANNOTATE(kind "(" #expr ") (" explain ")");          \
      MOZ_REALLY_CRASH(__LINE__);                                    \
    }                                                                \
  } while (false)

#define MOZ_ASSERT_GLUE(a, b) a b
#define MOZ_RELEASE_ASSERT(...)                                       \
  MOZ_ASSERT_GLUE(                                                    \
      MOZ_PASTE_PREFIX_AND_ARG_COUNT(MOZ_ASSERT_HELPER, __VA_ARGS__), \
      ("MOZ_RELEASE_ASSERT", __VA_ARGS__))

#ifdef DEBUG
#  define MOZ_ASSERT(...)                                               \
    MOZ_ASSERT_GLUE(                                                    \
        MOZ_PASTE_PREFIX_AND_ARG_COUNT(MOZ_ASSERT_HELPER, __VA_ARGS__), \
        ("MOZ_ASSERT", __VA_ARGS__))
#else
#  define MOZ_ASSERT(...) \
    do {                  \
    } while (false)
#endif /* DEBUG */

#if defined(NIGHTLY_BUILD) || defined(MOZ_DEV_EDITION) || defined(DEBUG)
#  define MOZ_DIAGNOSTIC_ASSERT(...)                                    \
    MOZ_ASSERT_GLUE(                                                    \
        MOZ_PASTE_PREFIX_AND_ARG_COUNT(MOZ_ASSERT_HELPER, __VA_ARGS__), \
        ("MOZ_DIAGNOSTIC_ASSERT", __VA_ARGS__))
#  define MOZ_DIAGNOSTIC_ASSERT_ENABLED 1
#else
#  define MOZ_DIAGNOSTIC_ASSERT(...) \
    do {                             \
    } while (false)
#endif

/*
 * MOZ_ASSERT_IF(cond1, cond2) is equivalent to MOZ_ASSERT(cond2) if cond1 is
 * true.
 *
 *   MOZ_ASSERT_IF(isPrime(num), num == 2 || isOdd(num));
 *
 * As with MOZ_ASSERT, MOZ_ASSERT_IF has effect only in debug builds.  It is
 * designed to catch bugs during debugging, not "in the field".
 */
#ifdef DEBUG
#  define MOZ_ASSERT_IF(cond, expr) \
    do {                            \
      if (cond) {                   \
        MOZ_ASSERT(expr);           \
      }                             \
    } while (false)
#else
#  define MOZ_ASSERT_IF(cond, expr) \
    do {                            \
    } while (false)
#endif

/*
 * MOZ_DIAGNOSTIC_ASSERT_IF is like MOZ_ASSERT_IF, but using
 * MOZ_DIAGNOSTIC_ASSERT as the underlying assert.
 *
 * See the block comment for MOZ_DIAGNOSTIC_ASSERT above for more details on how
 * diagnostic assertions work and how to use them.
 */
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
#  define MOZ_DIAGNOSTIC_ASSERT_IF(cond, expr) \
    do {                                       \
      if (cond) {                              \
        MOZ_DIAGNOSTIC_ASSERT(expr);           \
      }                                        \
    } while (false)
#else
#  define MOZ_DIAGNOSTIC_ASSERT_IF(cond, expr) \
    do {                                       \
    } while (false)
#endif

/*
 * MOZ_ASSUME_UNREACHABLE_MARKER() expands to an expression which states that
 * it is undefined behavior for execution to reach this point.  No guarantees
 * are made about what will happen if this is reached at runtime.  Most code
 * should use MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE because it has extra
 * asserts.
 */
#if defined(__clang__) || defined(__GNUC__)
#  define MOZ_ASSUME_UNREACHABLE_MARKER() __builtin_unreachable()
#elif defined(_MSC_VER)
#  define MOZ_ASSUME_UNREACHABLE_MARKER() __assume(0)
#else
#  ifdef __cplusplus
#    define MOZ_ASSUME_UNREACHABLE_MARKER() ::abort()
#  else
#    define MOZ_ASSUME_UNREACHABLE_MARKER() abort()
#  endif
#endif

/*
 * MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE([reason]) tells the compiler that it
 * can assume that the macro call cannot be reached during execution.  This lets
 * the compiler generate better-optimized code under some circumstances, at the
 * expense of the program's behavior being undefined if control reaches the
 * MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE.
 *
 * In Gecko, you probably should not use this macro outside of performance- or
 * size-critical code, because it's unsafe.  If you don't care about code size
 * or performance, you should probably use MOZ_ASSERT or MOZ_CRASH.
 *
 * SpiderMonkey is a different beast, and there it's acceptable to use
 * MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE more widely.
 *
 * Note that MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE is noreturn, so it's valid
 * not to return a value following a MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE
 * call.
 *
 * Example usage:
 *
 *   enum ValueType {
 *     VALUE_STRING,
 *     VALUE_INT,
 *     VALUE_FLOAT
 *   };
 *
 *   int ptrToInt(ValueType type, void* value) {
 *   {
 *     // We know for sure that type is either INT or FLOAT, and we want this
 *     // code to run as quickly as possible.
 *     switch (type) {
 *     case VALUE_INT:
 *       return *(int*) value;
 *     case VALUE_FLOAT:
 *       return (int) *(float*) value;
 *     default:
 *       MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Unexpected ValueType");
 *     }
 *   }
 */

/*
 * Unconditional assert in debug builds for (assumed) unreachable code paths
 * that have a safe return without crashing in release builds.
 */
#define MOZ_ASSERT_UNREACHABLE(reason) \
  MOZ_ASSERT(false, "MOZ_ASSERT_UNREACHABLE: " reason)

#define MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE(reason) \
  do {                                                  \
    MOZ_ASSERT_UNREACHABLE(reason);                     \
    MOZ_ASSUME_UNREACHABLE_MARKER();                    \
  } while (false)

/**
 * MOZ_FALLTHROUGH_ASSERT is an annotation to suppress compiler warnings about
 * switch cases that MOZ_ASSERT(false) (or its alias MOZ_ASSERT_UNREACHABLE) in
 * debug builds, but intentionally fall through in release builds to handle
 * unexpected values.
 *
 * Why do we need MOZ_FALLTHROUGH_ASSERT in addition to [[fallthrough]]? In
 * release builds, the MOZ_ASSERT(false) will expand to `do { } while (false)`,
 * requiring a [[fallthrough]] annotation to suppress a -Wimplicit-fallthrough
 * warning. In debug builds, the MOZ_ASSERT(false) will expand to something like
 * `if (true) { MOZ_CRASH(); }` and the [[fallthrough]] annotation will cause
 * a -Wunreachable-code warning. The MOZ_FALLTHROUGH_ASSERT macro breaks this
 * warning stalemate.
 *
 * // Example before MOZ_FALLTHROUGH_ASSERT:
 * switch (foo) {
 *   default:
 *     // This case wants to assert in debug builds, fall through in release.
 *     MOZ_ASSERT(false); // -Wimplicit-fallthrough warning in release builds!
 *     [[fallthrough]];   // but -Wunreachable-code warning in debug builds!
 *   case 5:
 *     return 5;
 * }
 *
 * // Example with MOZ_FALLTHROUGH_ASSERT:
 * switch (foo) {
 *   default:
 *     // This case asserts in debug builds, falls through in release.
 *     MOZ_FALLTHROUGH_ASSERT("Unexpected foo value?!");
 *   case 5:
 *     return 5;
 * }
 */
#ifdef DEBUG
#  define MOZ_FALLTHROUGH_ASSERT(...) \
    MOZ_CRASH("MOZ_FALLTHROUGH_ASSERT: " __VA_ARGS__)
#else
#  define MOZ_FALLTHROUGH_ASSERT(...) [[fallthrough]]
#endif

/*
 * MOZ_ALWAYS_TRUE(expr) and friends always evaluate the provided expression,
 * in debug builds and in release builds both.  Then, in debug builds and
 * Nightly and DevEdition release builds, the value of the expression is
 * asserted either true or false using MOZ_DIAGNOSTIC_ASSERT.
 */
#define MOZ_ALWAYS_TRUE(expr)              \
  do {                                     \
    if (MOZ_LIKELY(expr)) {                \
      /* Silence [[nodiscard]]. */         \
    } else {                               \
      MOZ_DIAGNOSTIC_ASSERT(false, #expr); \
    }                                      \
  } while (false)

#define MOZ_ALWAYS_FALSE(expr) MOZ_ALWAYS_TRUE(!(expr))
#define MOZ_ALWAYS_OK(expr) MOZ_ALWAYS_TRUE((expr).isOk())
#define MOZ_ALWAYS_ERR(expr) MOZ_ALWAYS_TRUE((expr).isErr())

#undef MOZ_DUMP_ASSERTION_STACK
#undef MOZ_CRASH_CRASHREPORT

#endif /* mozilla_Assertions_h */
