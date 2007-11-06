/* boost random/ranlux.hpp header file
 *
 * Copyright Jens Maurer 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: ranlux.hpp,v 1.5 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  created
 */

#ifndef BOOST_RANDOM_RANLUX_HPP
#define BOOST_RANDOM_RANLUX_HPP

#include <boost/config.hpp>
#include <boost/random/subtract_with_carry.hpp>
#include <boost/random/discard_block.hpp>

namespace boost {

namespace random {
  typedef subtract_with_carry<int, (1<<24), 10, 24, 0> ranlux_base;
  typedef subtract_with_carry_01<float, 24, 10, 24> ranlux_base_01;
  typedef subtract_with_carry_01<double, 48, 10, 24> ranlux64_base_01;
}

typedef random::discard_block<random::ranlux_base, 223, 24> ranlux3;
typedef random::discard_block<random::ranlux_base, 389, 24> ranlux4;

typedef random::discard_block<random::ranlux_base_01, 223, 24> ranlux3_01;
typedef random::discard_block<random::ranlux_base_01, 389, 24> ranlux4_01;

typedef random::discard_block<random::ranlux64_base_01, 223, 24> ranlux64_3_01;
typedef random::discard_block<random::ranlux64_base_01, 389, 24> ranlux64_4_01;

#if !defined(BOOST_NO_INT64_T) && !defined(BOOST_NO_INTEGRAL_INT64_T)
namespace random {
  typedef random::subtract_with_carry<int64_t, (int64_t(1)<<48), 10, 24, 0> ranlux64_base;
}
typedef random::discard_block<random::ranlux64_base, 223, 24> ranlux64_3;
typedef random::discard_block<random::ranlux64_base, 389, 24> ranlux64_4;
#endif /* !BOOST_NO_INT64_T && !BOOST_NO_INTEGRAL_INT64_T */

} // namespace boost

#endif // BOOST_RANDOM_LINEAR_CONGRUENTIAL_HPP
