// Boost.Function library
//  Copyright (C) Douglas Gregor 2008
//
//  Use, modification and distribution is subject to the Boost
//  Software License, Version 1.0.  (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org
#ifndef BOOST_FUNCTION_FWD_HPP
#define BOOST_FUNCTION_FWD_HPP
#include <boost/config.hpp>

namespace boost {
  class bad_function_call;

  // Preferred syntax
  template<typename Signature> class function;

  template<typename Signature>
  inline void swap(function<Signature>& f1, function<Signature>& f2)
  {
    f1.swap(f2);
  }

  // Portable syntax
  template<typename R, typename... T> class function_n;

  template<typename R, typename... T> using function0 = function_n<R, T...>;
  template<typename R, typename... T> using function1 = function_n<R, T...>;
  template<typename R, typename... T> using function2 = function_n<R, T...>;
  template<typename R, typename... T> using function3 = function_n<R, T...>;
  template<typename R, typename... T> using function4 = function_n<R, T...>;
  template<typename R, typename... T> using function5 = function_n<R, T...>;
  template<typename R, typename... T> using function6 = function_n<R, T...>;
  template<typename R, typename... T> using function7 = function_n<R, T...>;
  template<typename R, typename... T> using function8 = function_n<R, T...>;
  template<typename R, typename... T> using function9 = function_n<R, T...>;

  template<typename R, typename... T> using function10 = function_n<R, T...>;
  template<typename R, typename... T> using function11 = function_n<R, T...>;
  template<typename R, typename... T> using function12 = function_n<R, T...>;
  template<typename R, typename... T> using function13 = function_n<R, T...>;
  template<typename R, typename... T> using function14 = function_n<R, T...>;
  template<typename R, typename... T> using function15 = function_n<R, T...>;
  template<typename R, typename... T> using function16 = function_n<R, T...>;
  template<typename R, typename... T> using function17 = function_n<R, T...>;
  template<typename R, typename... T> using function18 = function_n<R, T...>;
  template<typename R, typename... T> using function19 = function_n<R, T...>;

  template<typename R, typename... T> using function20 = function_n<R, T...>;
  template<typename R, typename... T> using function21 = function_n<R, T...>;
  template<typename R, typename... T> using function22 = function_n<R, T...>;
  template<typename R, typename... T> using function23 = function_n<R, T...>;
  template<typename R, typename... T> using function24 = function_n<R, T...>;
  template<typename R, typename... T> using function25 = function_n<R, T...>;
  template<typename R, typename... T> using function26 = function_n<R, T...>;
  template<typename R, typename... T> using function27 = function_n<R, T...>;
  template<typename R, typename... T> using function28 = function_n<R, T...>;
  template<typename R, typename... T> using function29 = function_n<R, T...>;

  template<typename R, typename... T> using function30 = function_n<R, T...>;
}

#endif
