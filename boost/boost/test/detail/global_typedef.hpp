//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: global_typedef.hpp,v $
//
//  Version     : $Revision: 1.2 $
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

enum report_level  { CONFIRMATION_REPORT, SHORT_REPORT, DETAILED_REPORT, NO_REPORT, INV_REPORT_LEVEL };

//____________________________________________________________________________//

enum output_format { CLF /* compiler log format */, XML /* XML */ };

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

inline test_unit_type
test_id_2_unit_type( test_unit_id id )
{
    return (id & 0xFFFF0000) != 0 ? tut_case : tut_suite;
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: global_typedef.hpp,v $
//  Revision 1.2  2006/03/15 03:18:29  rogeeff
//  made literal resizable
//
//  Revision 1.1  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_GLOBAL_TYPEDEF_HPP_021005GER
