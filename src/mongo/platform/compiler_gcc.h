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

/**
 * Compiler-specific implementations for gcc.
 *
 * Refer to mongo/platform/compiler.h for usage documentation.
 */

#pragma once


#ifdef __clang__
// Our minimum clang version (3.4) doesn't support the "cold" attribute. We could try to use it with
// clang versions that support the attribute, but since Apple uses weird version numbers on clang
// and the main goal with the attribute is to improve our production builds with gcc, it didn't seem
// worth it.
#define MONGO_COMPILER_COLD_FUNCTION
#define MONGO_COMPILER_NORETURN __attribute__((__noreturn__))
#else
#define MONGO_COMPILER_COLD_FUNCTION __attribute__((__cold__))
#define MONGO_COMPILER_NORETURN __attribute__((__noreturn__, __cold__))
#endif

#define MONGO_COMPILER_VARIABLE_UNUSED __attribute__((__unused__))

#define MONGO_COMPILER_ALIGN_TYPE(ALIGNMENT) __attribute__((__aligned__(ALIGNMENT)))

#define MONGO_COMPILER_ALIGN_VARIABLE(ALIGNMENT) __attribute__((__aligned__(ALIGNMENT)))

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

#define MONGO_COMPILER_API_EXPORT __attribute__((__visibility__("default")))
#define MONGO_COMPILER_API_IMPORT
#define MONGO_COMPILER_API_CALLING_CONVENTION

#define MONGO_likely(x) static_cast<bool>(__builtin_expect(static_cast<bool>(x), 1))
#define MONGO_unlikely(x) static_cast<bool>(__builtin_expect(static_cast<bool>(x), 0))

#define MONGO_COMPILER_ALWAYS_INLINE [[gnu::always_inline]]

#define MONGO_COMPILER_UNREACHABLE __builtin_unreachable()
