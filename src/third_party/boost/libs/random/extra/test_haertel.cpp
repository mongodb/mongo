/* haertel.hpp file
 *
 * Copyright Jens Maurer 2000, 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * $Id$
 *
 * Revision history
 */

#include <string>
#include <iostream>

#include <boost/format.hpp>

#include "haertel.hpp"

#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

template<class Gen, class T>
inline void check_validation(Gen & gen, T value, const std::string & name)
{
  for(int i = 0; i < 100000-1; ++i)
    gen();

  typename Gen::result_type actual = gen();
  BOOST_CHECK_MESSAGE(value == actual, 
      boost::str(boost::format("%s: check value == gen() failed [%d != %d]") %
                 name % value % actual));
}

// we have validation after 100000 steps with Haertel's generators
template<class Gen, class T>
void validate(T value, const std::string & name)
{
  Gen gen(1234567);
  check_validation(gen, value, name);
}

BOOST_AUTO_TEST_CASE(test_haertel)
{
  using namespace Haertel;
  validate<LCG_Af2>(183269031u, "LCG_Af2");
  validate<LCG_Die1>(522319944u, "LCG_Die1");
  validate<LCG_Fis>(-2065162233u, "LCG_Fis");
  validate<LCG_FM>(581815473u, "LCG_FM");
  validate<LCG_Hae>(28931709, "LCG_Hae");
  validate<LCG_VAX>(1508154087u, "LCG_VAX");
  validate<NLG_Inv2>(6666884, "NLG_Inv2");
  validate<NLG_Inv4>(1521640076, "NLG_Inv4");
  validate<NLG_Inv5>(641840839, "NLG_Inv5");
  static const int acorn7_init[]
    = { 1234567, 7654321, 246810, 108642, 13579, 97531, 555555 };
  MRG_Acorn7 acorn7(acorn7_init);
  check_validation(acorn7, 874294697, "MRG_Acorn7");
  // This currently fails.  I don't want to touch it until
  // I trace the source of this generator. --SJW
  validate<MRG_Fib2>(1234567u, "MRG_Fib2");
}
