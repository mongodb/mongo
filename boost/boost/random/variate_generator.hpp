/* boost random/variate_generator.hpp header file
 *
 * Copyright Jens Maurer 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: variate_generator.hpp,v 1.8 2005/02/14 11:53:50 johnmaddock Exp $
 *
 */

#ifndef BOOST_RANDOM_RANDOM_GENERATOR_HPP
#define BOOST_RANDOM_RANDOM_GENERATOR_HPP

#include <boost/config.hpp>

// implementation details
#include <boost/detail/workaround.hpp>
#include <boost/random/uniform_01.hpp>
#include <boost/random/detail/pass_through_engine.hpp>
#include <boost/random/detail/uniform_int_float.hpp>
#include <boost/random/detail/ptr_helper.hpp>

// Borland C++ 5.6.0 has problems using its numeric_limits traits as
// template parameters
#if BOOST_WORKAROUND(__BORLANDC__, <= 0x564)
#include <boost/type_traits/is_integral.hpp>
#endif

namespace boost {

namespace random {
namespace detail {

template<bool have_int, bool want_int>
struct engine_helper;

// for consistency, always have two levels of decorations
template<>
struct engine_helper<true, true>
{
  template<class Engine, class DistInputType>
  struct impl
  {
    typedef pass_through_engine<Engine> type;
  };
};

template<>
struct engine_helper<false, false>
{
  template<class Engine, class DistInputType>
  struct impl
  {
    typedef uniform_01<Engine, DistInputType> type;
  };
};

template<>
struct engine_helper<true, false>
{
  template<class Engine, class DistInputType>
  struct impl
  {
    typedef uniform_01<Engine, DistInputType> type;
  };
};

template<>
struct engine_helper<false, true>
{
  template<class Engine, class DistInputType>
  struct impl
  {
    typedef uniform_int_float<Engine, unsigned long> type;
  };
};

} // namespace detail
} // namespace random


template<class Engine, class Distribution>
class variate_generator
{
private:
  typedef random::detail::pass_through_engine<Engine> decorated_engine;

public:
  typedef typename decorated_engine::base_type engine_value_type;
  typedef Engine engine_type;
  typedef Distribution distribution_type;
  typedef typename Distribution::result_type result_type;

  variate_generator(Engine e, Distribution d)
    : _eng(decorated_engine(e)), _dist(d) { }

  result_type operator()() { return _dist(_eng); }
  template<class T>
  result_type operator()(T value) { return _dist(_eng, value); }

  engine_value_type& engine() { return _eng.base().base(); }
  const engine_value_type& engine() const { return _eng.base().base(); }

  distribution_type& distribution() { return _dist; }
  const distribution_type& distribution() const { return _dist; }

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (distribution().min)(); }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (distribution().max)(); }

private:
#if BOOST_WORKAROUND(__BORLANDC__, <= 0x564)
  typedef typename random::detail::engine_helper<
    boost::is_integral<typename decorated_engine::result_type>::value,
    boost::is_integral<typename Distribution::input_type>::value
    >::BOOST_NESTED_TEMPLATE impl<decorated_engine, typename Distribution::input_type>::type internal_engine_type;
#else
  enum {
    have_int = std::numeric_limits<typename decorated_engine::result_type>::is_integer,
    want_int = std::numeric_limits<typename Distribution::input_type>::is_integer
  };
  typedef typename random::detail::engine_helper<have_int, want_int>::BOOST_NESTED_TEMPLATE impl<decorated_engine, typename Distribution::input_type>::type internal_engine_type;
#endif

  internal_engine_type _eng;
  distribution_type _dist;
};

} // namespace boost

#endif // BOOST_RANDOM_RANDOM_GENERATOR_HPP
