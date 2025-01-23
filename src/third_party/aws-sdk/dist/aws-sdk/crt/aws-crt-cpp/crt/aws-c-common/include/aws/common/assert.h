#ifndef AWS_COMMON_ASSERT_H
#define AWS_COMMON_ASSERT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/exports.h>
#include <aws/common/macros.h>
#include <stdio.h>

AWS_PUSH_SANE_WARNING_LEVEL
AWS_EXTERN_C_BEGIN

AWS_COMMON_API
AWS_DECLSPEC_NORETURN
void aws_fatal_assert(const char *cond_str, const char *file, int line) AWS_ATTRIBUTE_NORETURN;

AWS_EXTERN_C_END

#if defined(CBMC)
#    define AWS_PANIC_OOM(mem, msg)                                                                                    \
        do {                                                                                                           \
            if (!(mem)) {                                                                                              \
                fprintf(stderr, "%s: %s, line %d", msg, __FILE__, __LINE__);                                           \
                exit(-1);                                                                                              \
            }                                                                                                          \
        } while (0)
#else
#    define AWS_PANIC_OOM(mem, msg)                                                                                    \
        do {                                                                                                           \
            if (!(mem)) {                                                                                              \
                fprintf(stderr, "%s", msg);                                                                            \
                abort();                                                                                               \
            }                                                                                                          \
        } while (0)
#endif /* defined(CBMC) */

#if defined(CBMC)
#    define AWS_ASSUME(cond) __CPROVER_assume(cond)
#elif defined(_MSC_VER)
#    define AWS_ASSUME(cond) __assume(cond)
#    define AWS_UNREACHABLE() __assume(0)
#elif defined(__clang__)
#    define AWS_ASSUME(cond)                                                                                           \
        do {                                                                                                           \
            bool _result = (cond);                                                                                     \
            __builtin_assume(_result);                                                                                 \
        } while (false)
#    define AWS_UNREACHABLE() __builtin_unreachable()
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5))
#    define AWS_ASSUME(cond) ((cond) ? (void)0 : __builtin_unreachable())
#    define AWS_UNREACHABLE() __builtin_unreachable()
#else
#    define AWS_ASSUME(cond)
#    define AWS_UNREACHABLE()
#endif

#if defined(CBMC)
#    include <assert.h>
#    define AWS_ASSERT(cond) assert(cond)
#elif defined(DEBUG_BUILD) || defined(__clang_analyzer__)
#    define AWS_ASSERT(cond) AWS_FATAL_ASSERT(cond)
#else
#    define AWS_ASSERT(cond)
#endif /*  defined(CBMC) */

#if defined(CBMC)
#    define AWS_FATAL_ASSERT(cond) AWS_ASSERT(cond)
#elif defined(__clang_analyzer__)
#    define AWS_FATAL_ASSERT(cond)                                                                                     \
        if (!(cond)) {                                                                                                 \
            abort();                                                                                                   \
        }
#else
#    if defined(_MSC_VER)
#        define AWS_FATAL_ASSERT(cond)                                                                                 \
            __pragma(warning(push)) __pragma(warning(disable : 4127)) /* conditional expression is constant */         \
                if (!(cond)) {                                                                                         \
                aws_fatal_assert(#cond, __FILE__, __LINE__);                                                           \
            }                                                                                                          \
            __pragma(warning(pop))
#    else
#        define AWS_FATAL_ASSERT(cond)                                                                                 \
            do {                                                                                                       \
                if (!(cond)) {                                                                                         \
                    aws_fatal_assert(#cond, __FILE__, __LINE__);                                                       \
                }                                                                                                      \
            } while (0)
#    endif /* defined(_MSC_VER) */
#endif     /* defined(CBMC) */

/**
 * Define function contracts.
 * When the code is being verified using CBMC these contracts are formally verified;
 * When the code is built in debug mode, they are checked as much as possible using assertions
 * When the code is built in production mode, non-fatal contracts are not checked.
 * Violations of the function contracts are undefined behaviour.
 */
#ifdef CBMC
// clang-format off
// disable clang format, since it likes to break formatting of stringize macro.
// seems to be fixed in v15 plus, but we are not ready to update to it yet
#    define AWS_PRECONDITION2(cond, explanation) __CPROVER_precondition((cond), (explanation))
#    define AWS_PRECONDITION1(cond) __CPROVER_precondition((cond), #cond " check failed")
#    define AWS_FATAL_PRECONDITION2(cond, explanation) __CPROVER_precondition((cond), (explanation))
#    define AWS_FATAL_PRECONDITION1(cond) __CPROVER_precondition((cond), #cond " check failed")
#    define AWS_POSTCONDITION2(cond, explanation) __CPROVER_assert((cond), (explanation))
#    define AWS_POSTCONDITION1(cond) __CPROVER_assert((cond), #cond " check failed")
#    define AWS_FATAL_POSTCONDITION2(cond, explanation) __CPROVER_assert((cond), (explanation))
#    define AWS_FATAL_POSTCONDITION1(cond) __CPROVER_assert((cond), #cond " check failed")
#    define AWS_MEM_IS_READABLE_CHECK(base, len) (((len) == 0) || (__CPROVER_r_ok((base), (len))))
#    define AWS_MEM_IS_WRITABLE_CHECK(base, len) (((len) == 0) || (__CPROVER_r_ok((base), (len))))
// clang-format on
#else
#    define AWS_PRECONDITION2(cond, expl) AWS_ASSERT(cond)
#    define AWS_PRECONDITION1(cond) AWS_ASSERT(cond)
#    define AWS_FATAL_PRECONDITION2(cond, expl) AWS_FATAL_ASSERT(cond)
#    define AWS_FATAL_PRECONDITION1(cond) AWS_FATAL_ASSERT(cond)
#    define AWS_POSTCONDITION2(cond, expl) AWS_ASSERT(cond)
#    define AWS_POSTCONDITION1(cond) AWS_ASSERT(cond)
#    define AWS_FATAL_POSTCONDITION2(cond, expl) AWS_FATAL_ASSERT(cond)
#    define AWS_FATAL_POSTCONDITION1(cond) AWS_FATAL_ASSERT(cond)
/**
 * These macros should not be used in is_valid functions.
 * All validate functions are also used in assumptions for CBMC proofs,
 * which should not contain __CPROVER_*_ok primitives. The use of these primitives
 * in assumptions may lead to spurious results.
 * The C runtime does not give a way to check these properties,
 * but we can at least check that the pointer is valid. */
#    define AWS_MEM_IS_READABLE_CHECK(base, len) (((len) == 0) || (base))
#    define AWS_MEM_IS_WRITABLE_CHECK(base, len) (((len) == 0) || (base))
#endif /* CBMC */

/**
 * These macros can safely be used in validate functions.
 */
#define AWS_MEM_IS_READABLE(base, len) (((len) == 0) || (base))
#define AWS_MEM_IS_WRITABLE(base, len) (((len) == 0) || (base))

/* Logical consequence. */
#define AWS_IMPLIES(a, b) (!(a) || (b))

/**
 * If and only if (iff) is a biconditional logical connective between statements a and b.
 * We need double negations (!!) here to work correctly for non-Boolean a and b values.
 * Equivalent to (AWS_IMPLIES(a, b) && AWS_IMPLIES(b, a)).
 */
#define AWS_IFF(a, b) (!!(a) == !!(b))

#define AWS_RETURN_ERROR_IF_IMPL(type, cond, err, explanation)                                                         \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            return aws_raise_error(err);                                                                               \
        }                                                                                                              \
    } while (0)

#define AWS_RETURN_ERROR_IF3(cond, err, explanation) AWS_RETURN_ERROR_IF_IMPL("InternalCheck", cond, err, explanation)
#define AWS_RETURN_ERROR_IF2(cond, err) AWS_RETURN_ERROR_IF3(cond, err, #cond " check failed")
#define AWS_RETURN_ERROR_IF(...) CALL_OVERLOAD(AWS_RETURN_ERROR_IF, __VA_ARGS__)

#define AWS_ERROR_PRECONDITION3(cond, err, explanation) AWS_RETURN_ERROR_IF_IMPL("Precondition", cond, err, explanation)
#define AWS_ERROR_PRECONDITION2(cond, err) AWS_ERROR_PRECONDITION3(cond, err, #cond " check failed")
#define AWS_ERROR_PRECONDITION1(cond) AWS_ERROR_PRECONDITION2(cond, AWS_ERROR_INVALID_ARGUMENT)

#define AWS_ERROR_POSTCONDITION3(cond, err, explanation)                                                               \
    AWS_RETURN_ERROR_IF_IMPL("Postcondition", cond, err, explanation)
#define AWS_ERROR_POSTCONDITION2(cond, err) AWS_ERROR_POSTCONDITION3(cond, err, #cond " check failed")
#define AWS_ERROR_POSTCONDITION1(cond) AWS_ERROR_POSTCONDITION2(cond, AWS_ERROR_INVALID_ARGUMENT)

// The UNUSED is used to silence the complains of GCC for zero arguments in variadic macro
#define AWS_PRECONDITION(...) CALL_OVERLOAD(AWS_PRECONDITION, __VA_ARGS__)
#define AWS_FATAL_PRECONDITION(...) CALL_OVERLOAD(AWS_FATAL_PRECONDITION, __VA_ARGS__)
#define AWS_POSTCONDITION(...) CALL_OVERLOAD(AWS_POSTCONDITION, __VA_ARGS__)
#define AWS_FATAL_POSTCONDITION(...) CALL_OVERLOAD(AWS_FATAL_POSTCONDITION, __VA_ARGS__)
#define AWS_ERROR_PRECONDITION(...) CALL_OVERLOAD(AWS_ERROR_PRECONDITION, __VA_ARGS__)
#define AWS_ERROR_POSTCONDITION(...) CALL_OVERLOAD(AWS_ERROR_PRECONDITION, __VA_ARGS__)

#define AWS_RETURN_WITH_POSTCONDITION(_rval, ...)                                                                      \
    do {                                                                                                               \
        AWS_POSTCONDITION(__VA_ARGS__);                                                                                \
        return _rval;                                                                                                  \
    } while (0)

#define AWS_SUCCEED_WITH_POSTCONDITION(...) AWS_RETURN_WITH_POSTCONDITION(AWS_OP_SUCCESS, __VA_ARGS__)

#define AWS_OBJECT_PTR_IS_READABLE(ptr) AWS_MEM_IS_READABLE((ptr), sizeof(*(ptr)))
#define AWS_OBJECT_PTR_IS_WRITABLE(ptr) AWS_MEM_IS_WRITABLE((ptr), sizeof(*(ptr)))

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_ASSERT_H */
