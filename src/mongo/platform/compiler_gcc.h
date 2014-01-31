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

/**
 * Compiler-specific implementations for gcc.
 *
 * Refer to mongo/platform/compiler.h for usage documentation.
 */

#pragma once

#define MONGO_COMPILER_NORETURN __attribute__((__noreturn__))

#define MONGO_COMPILER_VARIABLE_UNUSED __attribute__((__unused__))

#define MONGO_COMPILER_ALIGN_TYPE(ALIGNMENT) __attribute__(( __aligned__(ALIGNMENT) ))

#define MONGO_COMPILER_ALIGN_VARIABLE(ALIGNMENT) __attribute__(( __aligned__(ALIGNMENT) ))

// NOTE(schwerin): These visibility and calling-convention macro definitions assume we're not using
// GCC/CLANG to target native Windows. If/when we decide to do such targeting, we'll need to change
// compiler flags on Windows to make sure we use an appropriate calling convention, and configure
// MONGO_COMPILER_API_EXPORT, MONGO_COMPILER_API_IMPORT and MONGO_COMPILER_API_CALLING_CONVENTION
// correctly.  I believe "correctly" is the following:
//
// #ifdef _WIN32
// #define MONGO_COMIPLER_API_EXPORT __attribute__(( __dllexport__ ))
// #define MONGO_COMPILER_API_IMPORT __attribute__(( __dllimport__ ))
// #ifdef _M_IX86
// #define MONGO_COMPILER_API_CALLING_CONVENTION __attribute__((__cdecl__))
// #else
// #define MONGO_COMPILER_API_CALLING_CONVENTION
// #endif
// #else ... fall through to the definitions below.

#define MONGO_COMPILER_API_EXPORT __attribute__(( __visibility__("default") ))
#define MONGO_COMPILER_API_IMPORT
#define MONGO_COMPILER_API_CALLING_CONVENTION
