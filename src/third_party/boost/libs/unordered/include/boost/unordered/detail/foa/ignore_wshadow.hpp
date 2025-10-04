/* Copyright 2023 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#include <boost/config.hpp>

#if defined(BOOST_GCC)
#if !defined(BOOST_UNORDERED_DETAIL_RESTORE_WSHADOW)
 /* GCC's -Wshadow triggers at scenarios like this: 
 *
 *   struct foo{};
 *   template<typename Base>
 *   struct derived:Base
 *   {
 *     void f(){int foo;}
 *   };
 * 
 *   derived<foo>x;
 *   x.f(); // declaration of "foo" in derived::f shadows base type "foo"
 *
 * This makes shadowing warnings unavoidable in general when a class template
 * derives from user-provided classes, as is the case with foa::table_core
 * deriving from empty_value.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#else
#pragma GCC diagnostic pop
#endif
#endif
