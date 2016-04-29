/*
 * Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

/**
 * Include "mongo/platform/compiler.h" to get compiler-targeted macro definitions and utilities.
 *
 * The following macros are provided in all compiler environments:
 *
 *
 * MONGO_COMPILER_COLD_FUNCTION
 *
 *   Informs the compiler that the function is cold. This can have the following effects:
 *   - The function is optimized for size over speed.
 *   - The function may be placed in a special cold section of the binary, away from other code.
 *   - Code paths that call this function are considered implicitly unlikely.
 *
 *
 * MONGO_COMPILER_NORETURN
 *
 *   Instructs the compiler that the decorated function will not return through the normal return
 *   path. All noreturn functions are also implicitly cold since they are either run-once code
 *   executed at startup or shutdown or code that handles errors by throwing an exception.
 *
 *   Correct: MONGO_COMPILER_NORETURN void myAbortFunction();
 *
 *
 * MONGO_COMPILER_VARIABLE_UNUSED
 *
 *   Instructs the compiler not to warn if it detects no use of the decorated variable.
 *   Typically only useful for variables that are always declared but only used in
 *   conditionally-compiled code.
 *
 *   Correct: MONGO_COMPILER_VARIABLE_UNUSED int ignored;
 *
 *
 * MONGO_COMPILER_ALIGN_TYPE(ALIGNMENT)
 *
 *   Instructs the compiler to use the given minimum alignment for the decorated type.
 *
 *   Alignments should probably always be powers of two.  Also, note that most allocators will not
 *   be able to guarantee better than 16- or 32-byte alignment.
 *
 *   Correct:
 *     class MONGO_COMPILER_ALIGN_TYPE(16) MyClass {...};
 *
 *   Incorrect:
 *     MONGO_COMPILER_ALIGN_TYPE(16) class MyClass {...};
 *     class MyClass{...} MONGO_COMPILER_ALIGN_TYPE(16);
 *
 *
 * MONGO_COMPILER_ALIGN_VARIABLE(ALIGNMENT)
 *
 *   Instructs the compiler to use the given minimum alignment for the decorated variable.
 *
 *   Note that most allocators will not allow heap allocated alignments that are better than 16- or
 *   32-byte aligned.  Stack allocators may only guarantee up to the natural word length worth of
 *   alignment.
 *
 *   Correct:
 *     class MyClass {
 *         MONGO_COMPILER_ALIGN_VARIABLE(8) char a;
 *     };
 *
 *     MONGO_COMPILER_ALIGN_VARIABLE(8) class MyClass {...} singletonInstance;
 *
 *   Incorrect:
 *     int MONGO_COMPILER_ALIGN_VARIABLE(16) a, b;
 *
 *
 * MONGO_COMPILER_API_EXPORT
 *
 *   Instructs the compiler to label the given type, variable or function as part of the
 *   exported interface of the library object under construction.
 *
 *   Correct:
 *       MONGO_COMPILER_API_EXPORT int globalSwitch;
 *       class MONGO_COMPILER_API_EXPORT ExportedType { ... };
 *       MONGO_COMPILER_API_EXPORT SomeType exportedFunction(...);
 *
 *   NOTE: Rather than using this macro directly, one typically declares another macro named
 *   for the library, which is conditionally defined to either MONGO_COMIPLER_API_EXPORT or
 *   MONGO_COMPILER_API_IMPORT based on whether the compiler is currently building the library
 *   or building an object that depends on the library, respectively.  For example,
 *   MONGO_FOO_API might be defined to MONGO_COMPILER_API_EXPORT when building the MongoDB
 *   libfoo shared library, and to MONGO_COMPILER_API_IMPORT when building an application that
 *   links against that shared library.
 *
 *
 * MONGO_COMPILER_API_IMPORT
 *
 *   Instructs the compiler to label the given type, variable or function as imported
 *   from another library, and not part of the library object under construction.
 *
 *   Same correct/incorrect usage as for MONGO_COMPILER_API_EXPORT.
 *
 *
 * MONGO_COMPILER_API_CALLING_CONVENTION
 *
 *    Explicitly decorates a function declaration the api calling convention used for
 *    shared libraries.
 *
 *    Same correct/incorrect usage as for MONGO_COMPILER_API_EXPORT.
 *
 *
 * MONGO_COMPILER_ALWAYS_INLINE
 *
 *    Overrides compiler heuristics to force that a particular function should always
 *    be inlined.
 *
 *
 * MONGO_COMPILER_UNREACHABLE
 *
 *    Tells the compiler that it can assume that this line will never execute. Unlike with
 *    MONGO_UNREACHABLE, there is no runtime check and reaching this macro is completely undefined
 *    behavior. It should only be used where it is provably impossible to reach, even in the face of
 *    adversarial inputs, but for some reason the compiler cannot figure this out on its own, for
 *    example after a call to a function that never returns but cannot be labeled with
 *    MONGO_COMPILER_NORETURN. In almost all cases MONGO_UNREACHABLE is preferred.
 */

#if defined(_MSC_VER)
#include "mongo/platform/compiler_msvc.h"
#elif defined(__GNUC__)
#include "mongo/platform/compiler_gcc.h"
#else
#error "Unsupported compiler family"
#endif
