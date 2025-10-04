///////////////////////////////////////////////////////////////////////////////
//  Copyright 2023 John Maddock.
//  Copyright Christopher Kormanyos 2013. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_MP_DETAIL_STRING_HELPERS_HPP
#define BOOST_MP_DETAIL_STRING_HELPERS_HPP

#include <algorithm>
#include <cstring>

namespace boost { namespace multiprecision { namespace detail {

   struct is_in_string
   {
      const char* begin;
      const char* end;
      is_in_string(const char* p) : begin(p), end(p + std::strlen(p)) {}

      bool operator()(char s) { return std::find(begin, end, s) != end; }
   };

   struct is_not_in_string
   {
      const char* begin;
      const char* end;
      is_not_in_string(const char* p) : begin(p), end(p + std::strlen(p)) {}

      bool operator()(char s) { return std::find(begin, end, s) == end; }
   };

   template <class Iterator>
   std::size_t find_first_of(Iterator begin, Iterator end, const char* what)
   {
      return static_cast<std::size_t>(std::find_if(begin, end, is_in_string(what)) - begin);
   }
   template <class Iterator>
   std::size_t find_first_not_of(Iterator begin, Iterator end, const char* what)
   {
      return static_cast<std::size_t>(std::find_if(begin, end, is_not_in_string(what)) - begin);
   }


}}} // namespace boost::multiprecision::detail

#endif // BOOST_MP_DETAIL_STRING_HELPERS_HPP
