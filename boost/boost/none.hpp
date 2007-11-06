// Copyright (C) 2003, Fernando Luis Cacciola Carballal.
// Copyright (C) 2007, Anthony Williams
// Copyright (C) 2007, Steven Watanabe, Richard Smith
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/lib/optional/ for documentation.
//
// You are welcome to contact the author at:
// fernando.cacciola@gmail.com
//
#ifndef BOOST_NONE_17SEP2003_HPP
#define BOOST_NONE_17SEP2003_HPP

namespace boost
{
  namespace detail
  {
    class none_helper;
  }

  inline void none(detail::none_helper);

  namespace detail
  {
    class none_helper
    {
    private:
      
      none_helper( none_helper const& ) {}
      
      friend void boost::none(none_helper);
    };
  }

  typedef void (*none_t)(detail::none_helper);

  inline void none(detail::none_helper) {}
}

#endif
