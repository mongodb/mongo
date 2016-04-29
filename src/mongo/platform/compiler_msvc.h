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
 * Compiler-specific implementations for MSVC.
 *
 * Refer to mongo/platform/compiler.h for usage documentation.
 */

#pragma once


// Microsoft seems opposed to implementing this:
// https://connect.microsoft.com/VisualStudio/feedback/details/804542
#define MONGO_COMPILER_COLD_FUNCTION

#define MONGO_COMPILER_NORETURN __declspec(noreturn)

#define MONGO_COMPILER_VARIABLE_UNUSED

#define MONGO_COMPILER_ALIGN_TYPE(ALIGNMENT) __declspec(align(ALIGNMENT))

#define MONGO_COMPILER_ALIGN_VARIABLE(ALIGNMENT) __declspec(align(ALIGNMENT))

#define MONGO_COMPILER_API_EXPORT __declspec(dllexport)
#define MONGO_COMPILER_API_IMPORT __declspec(dllimport)

#ifdef _M_IX86
// 32-bit x86 supports multiple of calling conventions.  We build supporting the cdecl convention
// (most common).  By labeling our exported and imported functions as such, we do a small favor to
// 32-bit Windows developers.
#define MONGO_COMPILER_API_CALLING_CONVENTION __cdecl
#else
#define MONGO_COMPILER_API_CALLING_CONVENTION
#endif

#define MONGO_likely(x) bool(x)
#define MONGO_unlikely(x) bool(x)

#define MONGO_COMPILER_ALWAYS_INLINE __forceinline

#define MONGO_COMPILER_UNREACHABLE __assume(false)
