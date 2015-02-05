//  boost/chrono/stopwatches/suspendable_stopwatch.hpp  ------------------------------------------------------------//
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_SUSPENDABLE_STOPWATCH__HPP
#define BOOST_CHRONO_STOPWATCHES_SUSPENDABLE_STOPWATCH__HPP

#include <boost/chrono/config.hpp>

#include <boost/chrono/stopwatches/stopwatch_scoped.hpp>
#include <boost/chrono/stopwatches/collectors/no_memory.hpp> // default laps_collector
#include <boost/chrono/stopwatches/dont_start.hpp>
#include <boost/chrono/detail/system.hpp>
#include <boost/chrono/system_clocks.hpp>
#include <utility>

namespace boost
{
  namespace chrono
  {

    template<typename Clock=high_resolution_clock, typename LapsCollector=no_memory<typename Clock::duration> >
    class suspendable_stopwatch
    {
    public:
      typedef LapsCollector laps_collector;
      typedef Clock clock;
      typedef typename Clock::duration duration;
      typedef typename Clock::time_point time_point;
      typedef typename Clock::rep rep;
      typedef typename Clock::period period;
      BOOST_STATIC_CONSTEXPR bool is_steady = Clock::is_steady;

      suspendable_stopwatch() :
        start_(duration::zero()),
        running_(false),
        suspended_(false),
        laps_collector_(),
        partial_(duration::zero())
      {
        start();
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit suspendable_stopwatch(
          system::error_code & ec
          ) :
        start_(duration::zero()),
        running_(false),
        suspended_(false),
        laps_collector_(),
        partial_(duration::zero())
      {
        start(ec);
      }
#endif

      explicit suspendable_stopwatch(
          const dont_start_t&
          ) :
          start_(duration::zero()),
          running_(false),
          suspended_(false),
          laps_collector_(),
          partial_(duration::zero())
      {
      }

      explicit suspendable_stopwatch(
          laps_collector const& acc
          ) :
          start_(duration::zero()),
          running_(false),
          suspended_(false),
          laps_collector_(acc),
          partial_(duration::zero())
      {
        start();
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit suspendable_stopwatch(
          laps_collector const& acc,
          system::error_code & ec
          ) :
          start_(duration::zero()),
          running_(false),
          suspended_(false),
          laps_collector_(acc),
          partial_(duration::zero())
      {
        start(ec);
      }
#endif

      suspendable_stopwatch(
          laps_collector const& acc,
          const dont_start_t&
          ) :
            start_(duration::zero()),
            running_(false),
            suspended_(false),
            laps_collector_(acc),
            partial_(duration::zero())
      {
      }

      ~suspendable_stopwatch()
      {
        stop();
      }

      void restart()
      {
        time_point tmp = clock::now();

        if (running_)
        {
          partial_ += tmp - start_;
          laps_collector_.store(partial_);
          partial_ = duration::zero();
        }
        else
        {
          running_ = true;
        }
        start_ = tmp;
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      void restart(system::error_code & ec)
      {
        time_point tmp = clock::now(ec);
        if (!BOOST_CHRONO_IS_THROWS(ec) && ec) return;

        if (running_)
        {
          partial_ += tmp - start_;
          laps_collector_.store(partial_);
          partial_ = duration::zero();
        }
        else
        {
          running_ = true;
        }
        start_ = tmp;
      }
#endif

      void start()
      {
          start_ = clock::now();;
          partial_ = duration::zero();
          running_ = true;
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      void start(system::error_code & ec)
      {
          time_point tmp = clock::now(ec);
          if (!BOOST_CHRONO_IS_THROWS(ec) && ec) return;

          partial_ = duration::zero();
          start_ = tmp;
          running_ = true;
      }
#endif

      void stop()
      {
          partial_ += clock::now() - start_;
          laps_collector_.store(partial_);
          start_ = time_point(duration::zero());
          running_ = false;
          suspended_ = false;
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      void stop(system::error_code & ec)
      {
          time_point tmp = clock::now(ec);
          if (!BOOST_CHRONO_IS_THROWS(ec) && ec) return;

          partial_ += tmp - start_;
          laps_collector_.store(partial_);
          start_ = time_point(duration::zero());
          running_ = false;
          suspended_ = false;
      }
#endif

      void suspend()
      {
        if (is_running())
        {
          if (!suspended_)
          {
            partial_ += clock::now() - start_;
            suspended_ = true;
          }
        }
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      void suspend(system::error_code & ec)
      {
        if (is_running())
        {
          if (!suspended_)
          {
            time_point tmp = clock::now(ec);
            if (!BOOST_CHRONO_IS_THROWS(ec) && ec) return;

            partial_ += tmp - start_;
            suspended_ = true;
          }
          else
          {
            ec.clear();
          }
        } else
        {
          ec.clear();
        }
      }
#endif

      void resume()
      {
        if (suspended_)
        {
          start_ = clock::now();
          suspended_ = false;
        }
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      void resume(system::error_code & ec)
      {
        if (suspended_)
        {
          time_point tmp = clock::now(ec);
          if (!BOOST_CHRONO_IS_THROWS(ec) && ec) return;

          start_ = tmp;
          suspended_ = false;
        } else
        {
          ec.clear();
        }
      }
#endif

      bool is_running() const {
        return running_;
      }
      bool is_suspended() const {
        return suspended_;
      }

      duration elapsed() const
      {
        if (is_running())
        {
          if (suspended_) {
            return partial_;
          }
          else
          {
            return partial_ + clock::now() - start_;
          }
        } else
        {
          return duration::zero();
        }
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      duration elapsed(system::error_code & ec) const
      {
        if (is_running())
        {
          if (suspended_) {
            return partial_;
          }
          else
          {
            time_point tmp = clock::now(ec);
            if (!BOOST_CHRONO_IS_THROWS(ec) && ec) return duration::zero();

            return partial_ + tmp - start_;
          }
        } else
        {
          return duration::zero();
        }
      }
#endif

      void reset(
          )
      {
        laps_collector_.reset();
        running_ = false;
        suspended_ = false;
        partial_ = duration::zero();
        start_ = time_point(duration::zero());
      }

      laps_collector const& get_laps_collector()
      {
        return laps_collector_;
      }


      typedef stopwatch_runner<suspendable_stopwatch<Clock, LapsCollector> >
          scoped_run;
      typedef stopwatch_stopper<suspendable_stopwatch<Clock, LapsCollector> >
          scoped_stop;
      typedef stopwatch_suspender<suspendable_stopwatch<Clock, LapsCollector> >
          scoped_suspend;
      typedef stopwatch_resumer<suspendable_stopwatch<Clock, LapsCollector> >
          scoped_resume;
    private:
      time_point start_;
      bool running_;
      bool suspended_;
      laps_collector laps_collector_;
      duration partial_;
    };

  } // namespace chrono
} // namespace boost

#endif
