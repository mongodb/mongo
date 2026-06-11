/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#pragma once

/*
 * The goal of s2n_result is to provide a strongly-typed error
 * signal value, which provides the compiler with enough information
 * to catch bugs.
 *
 * Historically, s2n has used int to signal errors. This has caused a few issues:
 *
 * ## GUARD in a function returning integer types
 *
 * There is no compiler error if `GUARD(nested_call());` is used in a function
 * that is meant to return integer type - not a error signal.
 *
 * ```c
 * uint8_t s2n_answer_to_the_ultimate_question() {
 *   POSIX_GUARD(s2n_sleep_for_years(7500000));
 *   return 42;
 * }
 * ```
 *
 * In this function we intended to return a `uint8_t` but used a
 * `GUARD` which will return -1 if the call fails. This can lead to
 * very subtle bugs.
 *
 * ## `GUARD`ing a function returning any integer type
 *
 * There is no compiler error if `GUARD(nested_call());` is used
 * on a function that doesn't actually return an error signal
 *
 * ```c
 * int s2n_deep_thought() {
 *   POSIX_GUARD(s2n_answer_to_the_ultimate_question());
 *   return 0;
 * }
 * ```
 *
 * In this function we intended guard against a failure of
 * `s2n_answer_to_the_ultimate_question` but that function doesn't
 * actually return an error signal. Again, this can lead to sublte
 * bugs.
 *
 * ## Ignored error signals
 *
 * Without the `warn_unused_result` function attribute, the compiler
 * provides no warning when forgetting to `GUARD` a function. Missing
 * a `GUARD` can lead to subtle bugs.
 *
 * ```c
 * int s2n_answer_to_the_ultimate_question() {
 *   s2n_sleep_for_years(7500000); // <- THIS SHOULD BE GUARDED!!!
 *   return 42;
 * }
 * ```
 *
 * # Solution
 *
 * s2n_result provides a newtype declaration, which is popular in
 * languages like [Haskell](https://wiki.haskell.org/Newtype) and
 * [Rust](https://doc.rust-lang.org/rust-by-example/generics/new_types.html).
 *
 * Functions that return S2N_RESULT are automatically marked with the
 * `warn_unused_result` attribute, which ensures they are GUARDed.
 */

#include <stdbool.h>

#include "api/s2n.h"

/* A value which indicates the outcome of a function */
typedef struct {
    int __error_signal;
} s2n_result;

/* used to signal a successful function return */
#define S2N_RESULT_OK ((s2n_result){ S2N_SUCCESS })

/* used to signal an error while executing a function */
#define S2N_RESULT_ERROR ((s2n_result){ S2N_FAILURE })

#if defined(__clang__) || defined(__GNUC__)
    #define S2N_RESULT_MUST_USE __attribute__((warn_unused_result))
#else
    #define S2N_RESULT_MUST_USE
#endif

/* returns true when the result is S2N_RESULT_OK */
S2N_RESULT_MUST_USE static inline bool s2n_result_is_ok(s2n_result result)
{
    return result.__error_signal == S2N_SUCCESS;
}

/* returns true when the result is S2N_RESULT_ERROR */
S2N_RESULT_MUST_USE static inline bool s2n_result_is_error(s2n_result result)
{
    return result.__error_signal != S2N_SUCCESS;
}

/**
 * Ignores the returned result of a function
 *
 * Generally, function results should always be checked. Using this function
 * could cause the system to behave in unexpected ways. As such, this function
 * should only be used in scenarios where the system state is not affected by
 * errors.
 */
static inline void s2n_result_ignore(s2n_result result)
{
    /* noop */
}

/* used in function declarations to signal function fallibility */
#define S2N_RESULT S2N_RESULT_MUST_USE s2n_result

/* The DEFER_CLEANUP macro discards the result of its cleanup function.
 * We need a version of s2n_result which can be ignored.
 */
#define S2N_CLEANUP_RESULT s2n_result
