//  boost/chrono/stopwatches/collectors/no_memory.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_MEMORIES_NO_MEMORY_HPP
#define BOOST_CHRONO_STOPWATCHES_MEMORIES_NO_MEMORY_HPP


namespace boost
{
  namespace chrono
  {

    template<typename Duration>
    struct no_memory
    {
      typedef Duration duration;

      duration elapsed() const  { return duration::zero(); }
      duration last() const  { return duration::zero(); }
      void store(duration const& )  {}
      void reset() {}

    };


  } // namespace chrono
} // namespace boost


#endif


