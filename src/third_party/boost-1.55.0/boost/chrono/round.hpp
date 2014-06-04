//  boost/chrono/round.hpp  ------------------------------------------------------------//

//  (C) Copyright Howard Hinnant
//  Copyright 2011 Vicente J. Botet Escriba

//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/chrono for documentation.

#ifndef BOOST_CHRONO_ROUND_HPP
#define BOOST_CHRONO_ROUND_HPP

#include <boost/chrono/duration.hpp>
#include <boost/chrono/duration.hpp>
//#include <boost/chrono/typeof/boost/chrono/chrono.hpp>

namespace boost
{
  namespace chrono
  {

    /**
     * rounds to nearest, to even on tie
     */
    template <class To, class Rep, class Period>
    To round(const duration<Rep, Period>& d)
    {
        To t0 = duration_cast<To>(d);
        To t1 = t0;
        ++t1;
#if 0
        // Avoid the user of BOOST_AUTO to make the library portable to Sun, PGI, ..
        BOOST_AUTO(diff0, d - t0);
        BOOST_AUTO(diff1, t1 - d);
#else
        typedef typename common_type<To, duration<Rep, Period> >::type  result_type;
        result_type diff0 = d - t0;
        result_type diff1 = t1 - d;
#endif
        if (diff0 == diff1)
        {
            if (t0.count() & 1)
                return t1;
            return t0;
        }
        else if (diff0 < diff1)
            return t0;
        return t1;
    }

  } // namespace chrono
} // namespace boost

#endif
