/* boost random/random_number_generator.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: random_number_generator.hpp,v 1.5 2004/11/09 21:22:00 jmaurer Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_RANDOM_NUMBER_GENERATOR_HPP
#define BOOST_RANDOM_RANDOM_NUMBER_GENERATOR_HPP

#include <boost/config.hpp>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/variate_generator.hpp>

namespace boost {

// a model for RandomNumberGenerator std:25.2.11 [lib.alg.random.shuffle]
template<class UniformRandomNumberGenerator, class IntType = long>
class random_number_generator
{
public:
  typedef UniformRandomNumberGenerator base_type;
  typedef IntType argument_type;
  typedef IntType result_type;
  random_number_generator(base_type& rng) : _rng(rng)
  { 
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(std::numeric_limits<result_type>::is_integer);
#endif
  }
  // compiler-generated copy ctor is fine
  // assignment is disallowed because there is a reference member

  result_type operator()(argument_type n)
  {
    typedef uniform_int<IntType> dist_type;
    return variate_generator<base_type&, dist_type>(_rng, dist_type(0, n-1))();
  }

private:
  base_type& _rng;
};

} // namespace boost

#endif // BOOST_RANDOM_RANDOM_NUMBER_GENERATOR_HPP
