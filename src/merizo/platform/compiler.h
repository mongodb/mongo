/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

/**
 * Include "merizo/platform/compiler.h" to get compiler-targeted macro definitions and utilities.
 *
 * The following macros are provided in all compiler environments:
 *
 *
 * MERIZO_COMPILER_COLD_FUNCTION
 *
 *   Informs the compiler that the function is cold. This can have the following effects:
 *   - The function is optimized for size over speed.
 *   - The function may be placed in a special cold section of the binary, away from other code.
 *   - Code paths that call this function are considered implicitly unlikely.
 *
 *
 * MERIZO_COMPILER_NORETURN
 *
 *   Instructs the compiler that the decorated function will not return through the normal return
 *   path. All noreturn functions are also implicitly cold since they are either run-once code
 *   executed at startup or shutdown or code that handles errors by throwing an exception.
 *
 *   Correct: MERIZO_COMPILER_NORETURN void myAbortFunction();
 *
 *
 * MERIZO_COMPILER_VARIABLE_UNUSED
 *
 *   Instructs the compiler not to warn if it detects no use of the decorated variable.
 *   Typically only useful for variables that are always declared but only used in
 *   conditionally-compiled code.
 *
 *   Correct: MERIZO_COMPILER_VARIABLE_UNUSED int ignored;
 *
 *
 * MERIZO_COMPILER_ALIGN_TYPE(ALIGNMENT)
 *
 *   Instructs the compiler to use the given minimum alignment for the decorated type.
 *
 *   Alignments should probably always be powers of two.  Also, note that most allocators will not
 *   be able to guarantee better than 16- or 32-byte alignment.
 *
 *   Correct:
 *     class MERIZO_COMPILER_ALIGN_TYPE(16) MyClass {...};
 *
 *   Incorrect:
 *     MERIZO_COMPILER_ALIGN_TYPE(16) class MyClass {...};
 *     class MyClass{...} MERIZO_COMPILER_ALIGN_TYPE(16);
 *
 *
 * MERIZO_COMPILER_ALIGN_VARIABLE(ALIGNMENT)
 *
 *   Instructs the compiler to use the given minimum alignment for the decorated variable.
 *
 *   Note that most allocators will not allow heap allocated alignments that are better than 16- or
 *   32-byte aligned.  Stack allocators may only guarantee up to the natural word length worth of
 *   alignment.
 *
 *   Correct:
 *     class MyClass {
 *         MERIZO_COMPILER_ALIGN_VARIABLE(8) char a;
 *     };
 *
 *     MERIZO_COMPILER_ALIGN_VARIABLE(8) class MyClass {...} singletonInstance;
 *
 *   Incorrect:
 *     int MERIZO_COMPILER_ALIGN_VARIABLE(16) a, b;
 *
 *
 * MERIZO_COMPILER_API_EXPORT
 *
 *   Instructs the compiler to label the given type, variable or function as part of the
 *   exported interface of the library object under construction.
 *
 *   Correct:
 *       MERIZO_COMPILER_API_EXPORT int globalSwitch;
 *       class MERIZO_COMPILER_API_EXPORT ExportedType { ... };
 *       MERIZO_COMPILER_API_EXPORT SomeType exportedFunction(...);
 *
 *   NOTE: Rather than using this macro directly, one typically declares another macro named
 *   for the library, which is conditionally defined to either MERIZO_COMIPLER_API_EXPORT or
 *   MERIZO_COMPILER_API_IMPORT based on whether the compiler is currently building the library
 *   or building an object that depends on the library, respectively.  For example,
 *   MERIZO_FOO_API might be defined to MERIZO_COMPILER_API_EXPORT when building the MerizoDB
 *   libfoo shared library, and to MERIZO_COMPILER_API_IMPORT when building an application that
 *   links against that shared library.
 *
 *
 * MERIZO_COMPILER_API_IMPORT
 *
 *   Instructs the compiler to label the given type, variable or function as imported
 *   from another library, and not part of the library object under construction.
 *
 *   Same correct/incorrect usage as for MERIZO_COMPILER_API_EXPORT.
 *
 *
 * MERIZO_COMPILER_API_CALLING_CONVENTION
 *
 *    Explicitly decorates a function declaration the api calling convention used for
 *    shared libraries.
 *
 *    Same correct/incorrect usage as for MERIZO_COMPILER_API_EXPORT.
 *
 *
 * MERIZO_COMPILER_ALWAYS_INLINE
 *
 *    Overrides compiler heuristics to force that a particular function should always
 *    be inlined.
 *
 *
 * MERIZO_COMPILER_UNREACHABLE
 *
 *    Tells the compiler that it can assume that this line will never execute. Unlike with
 *    MERIZO_UNREACHABLE, there is no runtime check and reaching this macro is completely undefined
 *    behavior. It should only be used where it is provably impossible to reach, even in the face of
 *    adversarial inputs, but for some reason the compiler cannot figure this out on its own, for
 *    example after a call to a function that never returns but cannot be labeled with
 *    MERIZO_COMPILER_NORETURN. In almost all cases MERIZO_UNREACHABLE is preferred.
 *
 *
 * MERIZO_WARN_UNUSED_RESULT_CLASS
 *
 *    Tells the compiler that a class defines a type for which checking results is necessary.  Types
 *    thus defined turn functions returning them into "must check results" style functions.  Preview
 *    of the `[[nodiscard]]` C++17 attribute.
 *
 *
 * MERIZO_WARN_UNUSED_RESULT_FUNCTION
 *
 *    Tells the compiler that a function returns a value for which consuming the result is
 *    necessary.  Functions thus defined are "must check results" style functions.  Preview of the
 *    `[[nodiscard]]` C++17 attribute.
 */


#if defined(_MSC_VER)
#include "merizo/platform/compiler_msvc.h"
#elif defined(__GNUC__)
#include "merizo/platform/compiler_gcc.h"
#else
#error "Unsupported compiler family"
#endif
