/*    Copyright 2013 10gen Inc.
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

// We need to drag in a C++ header so we can examine __GXX_EXPERIMENTAL_CXX0X__ or
// _LIBCPP_VERSION meaningfully. The <new> header is pretty lightweight, mostly unavoidable,
// and almost certain to bring in the standard library configuration macros.
#include <new>

// NOTE(acm): Before gcc-4.7, __cplusplus is always defined to be 1, so we can't reliably
// detect C++11 support by exclusively checking the value of __cplusplus.  Additionaly, libc++,
// whether in C++11 or C++03 mode, doesn't use TR1 and drops things into std instead.
#if __cplusplus >= 201103L || defined(__GXX_EXPERIMENTAL_CXX0X__) || defined(_LIBCPP_VERSION)

#include <functional>

#define MONGO_HASH_NAMESPACE_START namespace std {
#define MONGO_HASH_NAMESPACE_END }
#define MONGO_HASH_NAMESPACE std

#elif defined(_MSC_VER) && _MSC_VER >= 1500

#if _MSC_VER >= 1600  /* Visual Studio 2010+ */

#include <functional>

#define MONGO_HASH_NAMESPACE_START namespace std {
#define MONGO_HASH_NAMESPACE_END }
#define MONGO_HASH_NAMESPACE std

#else /* Older Visual Studio */

#include <tr1/functional>

#define MONGO_HASH_NAMESPACE_START namespace std { namespace tr1 {
#define MONGO_HASH_NAMESPACE_END }}
#define MONGO_HASH_NAMESPACE std::tr1

#endif

#elif defined(__GNUC__)

#include <tr1/functional>

#define MONGO_HASH_NAMESPACE_START namespace std { namespace tr1 {
#define MONGO_HASH_NAMESPACE_END }}
#define MONGO_HASH_NAMESPACE std::tr1

#else
#error "Cannot determine namespace for 'hash'"
#endif
