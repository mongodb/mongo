//  (C) Copyright Gennadiy Rozental 2001-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
//
//  Description : some trivial global typedefs
// ***************************************************************************

#ifndef BOOST_TEST_GLOBAL_TYPEDEF_HPP_021005GER
#define BOOST_TEST_GLOBAL_TYPEDEF_HPP_021005GER

#include <boost/test/utils/basic_cstring/basic_cstring.hpp>
#define BOOST_TEST_L( s )         boost::unit_test::const_string( s, sizeof( s ) - 1 )
#define BOOST_TEST_STRINGIZE( s ) BOOST_TEST_L( BOOST_STRINGIZE( s ) )
#define BOOST_TEST_EMPTY_STRING   BOOST_TEST_L( "" )

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

typedef unsigned long   counter_t;

//____________________________________________________________________________//

enum report_level  { INV_REPORT_LEVEL, CONFIRMATION_REPORT, SHORT_REPORT, DETAILED_REPORT, NO_REPORT };

//____________________________________________________________________________//

enum output_format { INV_OF, CLF /* compiler log format */, XML /* XML */ };

//____________________________________________________________________________//

enum test_unit_type { tut_case = 0x01, tut_suite = 0x10, tut_any = 0x11 };

//____________________________________________________________________________//

typedef unsigned long   test_unit_id;

const test_unit_id INV_TEST_UNIT_ID  = 0xFFFFFFFF;
const test_unit_id MAX_TEST_CASE_ID  = 0xFFFFFFFE;
const test_unit_id MIN_TEST_CASE_ID  = 0x00010000;
const test_unit_id MAX_TEST_SUITE_ID = 0x0000FF00;
const test_unit_id MIN_TEST_SUITE_ID = 0x00000001;

//____________________________________________________________________________//

namespace ut_detail {

inline test_unit_type
test_id_2_unit_type( test_unit_id id )
{
    return (id & 0xFFFF0000) != 0 ? tut_case : tut_suite;
}

//____________________________________________________________________________//

// helper templates to prevent ODR violations 
template<class T> 
struct static_constant { 
    static T value; 
}; 

template<class T> 
T static_constant<T>::value; 

//____________________________________________________________________________// 

} // namespace ut_detail

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_GLOBAL_TYPEDEF_HPP_021005GER
