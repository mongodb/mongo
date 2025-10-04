// Copyright (C) 2005, Fernando Luis Cacciola Carballal.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/optional for documentation.
//
// You are welcome to contact the author at:
//  fernando_cacciola@hotmail.com
//
#ifndef BOOST_OPTIONAL_OPTIONAL_IO_FLC_19NOV2002_HPP
#define BOOST_OPTIONAL_OPTIONAL_IO_FLC_19NOV2002_HPP

#ifndef BOOST_NO_IOSTREAM
#include <istream>
#include <ostream>

#include "boost/none.hpp"
#include "boost/optional/optional.hpp"


namespace boost
{

template<class CharType, class CharTrait>
inline
std::basic_ostream<CharType, CharTrait>&
operator<<(std::basic_ostream<CharType, CharTrait>& out, none_t)
{
  if (out.good())
  {
    out << "--";
  }

  return out;
}

template<class CharType, class CharTrait, class T>
inline
std::basic_ostream<CharType, CharTrait>&
operator<<(std::basic_ostream<CharType, CharTrait>& out, optional<T> const& v)
{
  if (out.good())
  {
    if (!v)
         out << "--" ;
    else out << ' ' << *v ;
  }

  return out;
}

template<class CharType, class CharTrait, class T>
inline
std::basic_istream<CharType, CharTrait>&
operator>>(std::basic_istream<CharType, CharTrait>& in, optional<T>& v)
{
  if (in.good())
  {
    int d = in.get();
    if (d == ' ')
    {
      T x;
      in >> x;
      v = optional_detail::move(x);
    }
    else
    {
      if (d == '-')
      {
        d = in.get();

        if (d == '-')
        {
          v = none;
          return in;
        }
      }

      in.setstate( std::ios::failbit );
    }
  }

  return in;
}

} // namespace boost

#endif // BOOST_NO_IOSTREAM
#endif
