/* Copyright 2024 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_FOA_CUMULATIVE_STATS_HPP
#define BOOST_UNORDERED_DETAIL_FOA_CUMULATIVE_STATS_HPP

#include <array>
#include <boost/config.hpp>
#include <boost/mp11/tuple.hpp>
#include <cmath>
#include <cstddef>

#if defined(BOOST_HAS_THREADS)
#include <boost/unordered/detail/foa/rw_spinlock.hpp>
#include <mutex>
#endif

namespace boost{
namespace unordered{
namespace detail{
namespace foa{

/* Cumulative one-pass calculation of the average, variance and deviation of
 * running sequences.
 */

struct sequence_stats_data
{
  double m=0.0;
  double m_prior=0.0;
  double s=0.0;
};

struct welfords_algorithm /* 0-based */
{
  template<typename T>
  int operator()(T&& x,sequence_stats_data& d)const noexcept
  {
    static_assert(
      noexcept(static_cast<double>(x)),
      "Argument conversion to double must not throw.");

    d.m_prior=d.m;
    d.m+=(static_cast<double>(x)-d.m)/static_cast<double>(n);
    d.s+=(n!=1)*
      (static_cast<double>(x)-d.m_prior)*(static_cast<double>(x)-d.m);

    return 0; /* mp11::tuple_transform requires that return type not be void */
  }

  std::size_t n;
};

struct sequence_stats_summary
{
  double average;
  double variance;
  double deviation;
};

/* Stats calculated jointly for N same-sized sequences to save the space
 * for count.
 */

template<std::size_t N>
class cumulative_stats
{
public:
  struct summary
  {
    std::size_t                          count;
    std::array<sequence_stats_summary,N> sequence_summary;
  };

  void reset()noexcept{*this=cumulative_stats();}
  
  template<typename... Ts>
  void add(Ts&&... xs)noexcept
  {
    static_assert(
      sizeof...(Ts)==N,"A sample must be provided for each sequence.");

    if(BOOST_UNLIKELY(++n==0)){ /* wraparound */
      reset();
      n=1;
    }
    mp11::tuple_transform(
      welfords_algorithm{n},
      std::forward_as_tuple(std::forward<Ts>(xs)...),
      data);
  }
  
  summary get_summary()const noexcept
  {
    summary res;
    res.count=n;
    for(std::size_t i=0;i<N;++i){
      double average=data[i].m,
             variance=n!=0?data[i].s/static_cast<double>(n):0.0, /* biased */
             deviation=std::sqrt(variance);
      res.sequence_summary[i]={average,variance,deviation};
    }
    return res;
  }

private:
  std::size_t                       n=0;
  std::array<sequence_stats_data,N> data;
};

#if defined(BOOST_HAS_THREADS)

template<std::size_t N>
class concurrent_cumulative_stats:cumulative_stats<N>
{
  using super=cumulative_stats<N>;
  using lock_guard=std::lock_guard<rw_spinlock>;

public:
  using summary=typename super::summary;

  concurrent_cumulative_stats()noexcept:super{}{}
  concurrent_cumulative_stats(const concurrent_cumulative_stats& x)noexcept:
    concurrent_cumulative_stats{x,lock_guard{x.mut}}{}

  concurrent_cumulative_stats&
  operator=(const concurrent_cumulative_stats& x)noexcept
  {
    auto x1=x;
    lock_guard lck{mut};
    static_cast<super&>(*this)=x1;
    return *this;
  }

  void reset()noexcept
  {
    lock_guard lck{mut};
    super::reset();
  }
  
  template<typename... Ts>
  void add(Ts&&... xs)noexcept
  {
    lock_guard lck{mut};
    super::add(std::forward<Ts>(xs)...);
  }
  
  summary get_summary()const noexcept
  {
    lock_guard lck{mut};
    return super::get_summary();
  }

private:
  concurrent_cumulative_stats(const super& x,lock_guard&&):super{x}{}

  mutable rw_spinlock mut;
};

#else

template<std::size_t N>
using concurrent_cumulative_stats=cumulative_stats<N>;

#endif

} /* namespace foa */
} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif
