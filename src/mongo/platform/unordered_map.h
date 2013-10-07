/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

// We need to drag in a C++ header so we can examine __GXX_EXPERIMENTAL_CXX0X__ or
// _LIBCPP_VERSION meaningfully. The <new> header is pretty lightweight, mostly unavoidable,
// and almost certain to bring in the standard library configuration macros.
#include <new>

// NOTE(acm): Before gcc-4.7, __cplusplus is always defined to be 1, so we can't reliably
// detect C++11 support by exclusively checking the value of __cplusplus. Additionaly, libc++,
// whether in C++11 or C++03 mode, doesn't use TR1 and drops things into std instead.
#if __cplusplus >= 201103L || defined(__GXX_EXPERIMENTAL_CXX0X__) || defined(_LIBCPP_VERSION)

#include <unordered_map>

namespace mongo {

    using std::unordered_map;

}  // namespace mongo

#elif defined(_MSC_VER) && _MSC_VER >= 1500

#include <unordered_map>

namespace mongo {

#if _MSC_VER >= 1600  /* Visual Studio 2010+ */
    using std::unordered_map;
#else
    using std::tr1::unordered_map;
#endif

}  // namespace mongo

#elif defined(__GNUC__)

#include <tr1/unordered_map>

namespace mongo {

    using std::tr1::unordered_map;

}  // namespace mongo

#else
#error "Compiler's standard library does not provide a C++ unordered_map implementation."
#endif
