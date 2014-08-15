//  boost/chrono/stopwatches/dont.hpp  ------------------------------------------------------------//
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_DONT_START__HPP
#define BOOST_CHRONO_STOPWATCHES_DONT_START__HPP

namespace boost
{
  namespace chrono
  {

    /**
     * Type used to don't start a basic_stopwatch at construction time.
     */
    struct dont_start_t
    {
    };

    /**
     * Instance used to don't start a basic_stopwatch at construction time.
     */
    static const dont_start_t dont_start =
    { };


  } // namespace chrono
} // namespace boost

#endif
