/*
 * Copyright 2012 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/**
 * Include "mongo/platform/compiler.h" to get compiler-targeted macro definitions and utilities.
 *
 * The following macros are provided in all compiler environments:
 *
 *
 * MONGO_COMPILER_NORETURN
 *
 *   Instructs the compiler that the decorated function will not return through the normal return
 *   path.
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
 *   NOTE: Rather than using this macro directly, one typically declares another macro named for the
 *   library, which is conditionally defined to either MONGO_COMIPLER_API_EXPORT or
 *   MONGO_COMPILER_API_IMPORT based on whether the compiler is currently building the library or
 *   building an object that depends on the library, respectively.  For example, MONGO_CLIENT_API
 *   might be defined to MONGO_COMPILER_API_EXPORT when building the MongoDB shared library, and to
 *   MONGO_COMPILER_API_IMPORT when building an application that links against the shared library.
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
 */

#if defined(_MSC_VER)
#include "mongo/platform/compiler_msvc.h"
#elif defined(__GNUC__)
#include "mongo/platform/compiler_gcc.h"
#else
#error "Unsupported compiler family"
#endif
