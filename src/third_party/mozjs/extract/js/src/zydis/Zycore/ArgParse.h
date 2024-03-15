/***************************************************************************************************

  Zyan Core Library (Zycore-C)

  Original Author : Joel Hoener

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Implements command-line argument parsing.
 */

#ifndef ZYCORE_ARGPARSE_H
#define ZYCORE_ARGPARSE_H

#include "zydis/Zycore/Types.h"
#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Vector.h"
#include "zydis/Zycore/String.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Structs and other types                                                                        */
/* ============================================================================================== */

/**
 * Definition of a single argument.
 */
typedef struct ZyanArgParseDefinition_
{
    /**
     * The argument name, e.g. `--help`.
     *
     * Must start with either one or two dashes. Single dash arguments must consist of a single
     * character, (e.g. `-n`), double-dash arguments can be of arbitrary length.
     */
    const char* name;
    /**
     * Whether the argument is boolean or expects a value.
     */
    ZyanBool boolean;
    /**
     * Whether this argument is required (error if missing).
     */
    ZyanBool required;
} ZyanArgParseDefinition;

/**
 * Configuration for argument parsing.
 */
typedef struct ZyanArgParseConfig_
{
    /**
     * `argv` argument passed to `main` by LibC.
     */
    const char** argv;
    /**
     * `argc` argument passed to `main` by LibC.
     */
    ZyanUSize argc;
    /**
     * Minimum # of accepted unnamed / anonymous arguments.
     */
    ZyanUSize min_unnamed_args;
    /**
     * Maximum # of accepted unnamed / anonymous arguments.
     */
    ZyanUSize max_unnamed_args;
    /**
     * Argument definition array, or `ZYAN_NULL`.
     *
     * Expects a pointer to an array of `ZyanArgParseDefinition` instances. The array is
     * terminated by setting the `.name` field of the last element to `ZYAN_NULL`. If no named
     * arguments should be parsed, you can also set this to `ZYAN_NULL`.
     */
    ZyanArgParseDefinition* args;
} ZyanArgParseConfig;

/**
 * Information about a parsed argument.
 */
typedef struct ZyanArgParseArg_
{
    /**
     * Corresponding argument definition, or `ZYAN_NULL` for unnamed args.
     *
     * This pointer is borrowed from the `cfg` pointer passed to `ZyanArgParse`.
     */
    const ZyanArgParseDefinition* def;
    /**
     * Whether the argument has a value (is non-boolean).
     */
    ZyanBool has_value;
    /**
     * If `has_value == true`, then the argument value.
     *
     * This is a view into the `argv` string array passed to `ZyanArgParse` via the `cfg` argument.
     */
    ZyanStringView value;
} ZyanArgParseArg;

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

#ifndef ZYAN_NO_LIBC

/**
 * Parse arguments according to a `ZyanArgParseConfig` definition.
 *
 * @param  cfg          Argument parser config to use.
 * @param  parsed       Receives the parsed output. Vector of `ZyanArgParseArg`. Ownership is
 *                      transferred to the user. Input is expected to be uninitialized. On error,
 *                      the vector remains uninitialized.
 * @param  error_token  On error, if it makes sense, receives the argument fragment causing the
 *                      error. Optional, may be `ZYAN_NULL`. The pointer borrows into the `cfg`
 *                      struct and doesn't have to be freed by the user.
 *
 * @return A `ZyanStatus` status determining whether the parsing succeeded.
 */
ZYCORE_EXPORT ZyanStatus ZyanArgParse(const ZyanArgParseConfig *cfg, ZyanVector* parsed,
    const char** error_token);

#endif

/**
 * Parse arguments according to a `ZyanArgParseConfig` definition.
 *
 * This version allows specification of a custom memory allocator and thus supports no-libc.
 *
 * @param  cfg          Argument parser config to use.
 * @param  parsed       Receives the parsed output. Vector of `ZyanArgParseArg`. Ownership is
 *                      transferred to the user. Input is expected to be uninitialized. On error,
 *                      the vector remains uninitialized.
 * @param  error_token  On error, if it makes sense, receives the argument fragment causing the
 *                      error. Optional, may be `ZYAN_NULL`. The pointer borrows into the `cfg`
 *                      struct and doesn't have to be freed by the user.
 * @param   allocator   The `ZyanAllocator` to be used for allocating the output vector's data.
 *
 * @return A `ZyanStatus` status determining whether the parsing succeeded.
 */
ZYCORE_EXPORT ZyanStatus ZyanArgParseEx(const ZyanArgParseConfig *cfg, ZyanVector* parsed,
    const char** error_token, ZyanAllocator* allocator);

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_ARGPARSE_H */
