/* boost random.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/random for documentation.
 *
 * $Id: random.hpp,v 1.18 2004/07/27 03:43:27 dgregor Exp $
 *
 * Revision history
 *  2000-02-18  portability fixes (thanks to Beman Dawes)
 *  2000-02-21  shuffle_output, inversive_congruential_schrage,
 *              generator_iterator, uniform_smallint
 *  2000-02-23  generic modulus arithmetic helper, removed *_schrage classes,
 *              implemented Streamable and EqualityComparable concepts for 
 *              generators, added Bernoulli distribution and Box-Muller
 *              transform
 *  2000-03-01  cauchy, lognormal, triangle distributions; fixed 
 *              uniform_smallint; renamed gaussian to normal distribution
 *  2000-03-05  implemented iterator syntax for distribution functions
 *  2000-04-21  removed some optimizations for better BCC/MSVC compatibility
 *  2000-05-10  adapted to BCC and MSVC
 *  2000-06-13  incorporated review results
 *  2000-07-06  moved basic templates from namespace detail to random
 *  2000-09-23  warning removals and int64 fixes (Ed Brey)
 *  2000-09-24  added lagged_fibonacci generator (Matthias Troyer)
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_HPP
#define BOOST_RANDOM_HPP

// generators
#include <boost/random/linear_congruential.hpp>
#include <boost/random/additive_combine.hpp>
#include <boost/random/inversive_congruential.hpp>
#include <boost/random/shuffle_output.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/lagged_fibonacci.hpp>
#include <boost/random/ranlux.hpp>
#include <boost/random/linear_feedback_shift.hpp>
#include <boost/random/xor_combine.hpp>

namespace boost {
  typedef random::xor_combine<random::xor_combine<random::linear_feedback_shift<uint32_t, 32, 31, 13, 12, 0>, 0,
    random::linear_feedback_shift<uint32_t, 32, 29, 2, 4, 0>, 0, 0>, 0,
                      random::linear_feedback_shift<uint32_t, 32, 28, 3, 17, 0>, 0, 0> taus88;
} // namespace  boost

// misc
#include <boost/random/random_number_generator.hpp>

// distributions
#include <boost/random/uniform_smallint.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/uniform_01.hpp>
#include <boost/random/uniform_real.hpp>
#include <boost/random/triangle_distribution.hpp>
#include <boost/random/bernoulli_distribution.hpp>
#include <boost/random/cauchy_distribution.hpp>
#include <boost/random/exponential_distribution.hpp>
#include <boost/random/geometric_distribution.hpp>
#include <boost/random/normal_distribution.hpp>
#include <boost/random/lognormal_distribution.hpp>
#include <boost/random/poisson_distribution.hpp>
#include <boost/random/gamma_distribution.hpp>
#include <boost/random/binomial_distribution.hpp>
#include <boost/random/uniform_on_sphere.hpp>

#endif // BOOST_RANDOM_HPP
