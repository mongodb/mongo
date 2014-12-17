//  boost/chrono/stopwatches/collectors/laps_sequence_container.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_MEMORIES_LAPS_CONTAINER_HPP
#define BOOST_CHRONO_STOPWATCHES_MEMORIES_LAPS_CONTAINER_HPP

#include <list>

namespace boost
{
  namespace chrono
  {

    template<
      typename Duration,
      typename SequenceContainer = std::list<Duration>
    >
    struct laps_sequence_container
    {
      typedef Duration duration;
      typedef typename duration::rep rep;
      typedef SequenceContainer storage_type;
      typedef typename SequenceContainer::iterator iterator;
      typedef typename SequenceContainer::const_iterator const_iterator;
      storage_type cont_;

      void store(duration const& d)
      {
        cont_.push_front(d);
      }

      void reset()
      {
        cont_.clear();
      }

      storage_type const& container() const  { return cont_; }

      duration last() const  {
        if (cont_.empty())
          return duration::zero();
        else
          return *cont_.begin();
      }

      duration elapsed() const  {
        duration elapsed_ = duration::zero();
        for (const_iterator it = cont_.begin(); it !=cont_.end(); ++it) elapsed_ += *it;
        return elapsed_;
      }

    };


  } // namespace chrono
} // namespace boost


#endif


