// @file mongo/platform/compiler_msvc.h

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
 * Compiler-specific implementations for MSVC.
 */

/**
 * Use this to decorate the declaration of functions that will not return.
 *
 * Correct:
 *    MONGO_COMPILER_NORETURN void myAbortFunction();
 */
#define MONGO_COMPILER_NORETURN __declspec(noreturn)

/**
 * Use this to decorate unused variable declarations.
 */
#define MONGO_COMPILER_VARIABLE_UNUSED

/**
 * Use this on a type declaration to specify its minimum alignment.
 *
 * Alignments should probably always be powers of two.  Also, note that most allocators will not be
 * able to guarantee better than 16- or 32-byte alignment.
 *
 * Correct:
 *    class MONGO_COMPILER_ALIGN_TYPE(16) MyClass {...};
 *
 * Incorrect:
 *    MONGO_COMPILER_ALIGN_TYPE(16) class MyClass {...};
 *    class MyClass{...} MONGO_COMPILER_ALIGN_TYPE(16);
 */
#define MONGO_COMPILER_ALIGN_TYPE(ALIGNMENT) __declspec( align( ALIGNMENT ) )

/**
 * Use this on a global variable or structure field declaration to specify that it must be allocated
 * at a location that is aligned to a multiple of "ALIGNMENT" bytes.
 *
 * Note that most allocators will not allow heap allocated alignments that are better than 16- or
 * 32-byte aligned.  Stack allocators may only guarantee up to the natural word length worth of
 * alignment.
 *
 * Correct:
 *    class MyClass {
 *        MONGO_COMPILER_ALIGN_VARIABLE(8) char a;
 *    };
 *
 *    MONGO_COMPILER_ALIGN_VARIABLE class MyClass {...} singletonInstance;
 *
 * Incorrect:
 *    int MONGO_COMPILER_ALIGN_VARIABLE(16) a, b;
 */
#define MONGO_COMPILER_ALIGN_VARIABLE(ALIGNMENT) __declspec( align( ALIGNMENT ) )
