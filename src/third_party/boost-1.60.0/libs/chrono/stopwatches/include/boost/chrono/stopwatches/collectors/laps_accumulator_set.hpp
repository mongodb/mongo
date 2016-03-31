//  boost/chrono/stopwatches/collectors/laps_accumulator_set.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_MEMORIES_LAPS_ACCUMULATOR_SET_HPP
#define BOOST_CHRONO_STOPWATCHES_MEMORIES_LAPS_ACCUMULATOR_SET_HPP

#include <boost/chrono/stopwatches/collectors/last_lap.hpp>
#include <boost/accumulators/framework/accumulator_set.hpp>
#include <boost/accumulators/statistics/count.hpp>
#include <boost/accumulators/statistics/sum.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/framework/features.hpp>

namespace boost
{
  namespace chrono
  {

    template<
      typename Duration,
      typename Features = accumulators::features<accumulators::tag::count,
        accumulators::tag::sum, accumulators::tag::min,
        accumulators::tag::max, accumulators::tag::mean>,
      typename Weight = void>
    struct laps_accumulator_set : last_lap<Duration>
    {
      typedef last_lap<Duration> base_type;
      typedef Duration duration;
      typedef typename duration::rep rep;
      typedef accumulators::accumulator_set<rep, Features,
          Weight> storage_type;
      storage_type acc_;

      void store(duration const& d)
      {
        this->base_type::store(d);
        acc_(d.count());
      }

      void reset()
      {
        this->base_type::reset();
        acc_ = storage_type();
      }

      storage_type const& accumulator_set() const  { return acc_; }

      duration elapsed() const  { return duration(accumulators::sum(acc_)); }

    };


  } // namespace chrono
} // namespace boost


#endif


