//  boost/chrono/stopwatches/collectors/last_lap.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_MEMORIES_LAST_LAP_HPP
#define BOOST_CHRONO_STOPWATCHES_MEMORIES_LAST_LAP_HPP


namespace boost
{
  namespace chrono
  {

    template<typename Duration>
    struct last_lap
    {
      typedef Duration duration;
      duration last_;
      void store(duration const& d)
      {
        last_ = d;
      }
      void reset()
      {
        last_ = duration::zero();
      }
      duration last() const  { return last_; }
      duration elapsed() const  { return duration::zero(); }

    };


  } // namespace chrono
} // namespace boost


#endif


