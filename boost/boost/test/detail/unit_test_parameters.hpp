//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test_parameters.hpp,v $
//
//  Version     : $Revision: 1.23.2.1 $
//
//  Description : storage for unit test framework parameters information
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_PARAMETERS_HPP_071894GER
#define BOOST_TEST_UNIT_TEST_PARAMETERS_HPP_071894GER

#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/detail/log_level.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                 runtime_config               ************** //
// ************************************************************************** //

namespace runtime_config {

void                    BOOST_TEST_DECL init( int* argc, char** argv );

unit_test::log_level    BOOST_TEST_DECL log_level();
bool                    BOOST_TEST_DECL no_result_code();
unit_test::report_level BOOST_TEST_DECL report_level();
const_string            BOOST_TEST_DECL test_to_run();
const_string            BOOST_TEST_DECL break_exec_path();
bool                    BOOST_TEST_DECL save_pattern();
bool                    BOOST_TEST_DECL show_build_info();
bool                    BOOST_TEST_DECL show_progress();
bool                    BOOST_TEST_DECL catch_sys_errors();
output_format           BOOST_TEST_DECL report_format();
output_format           BOOST_TEST_DECL log_format();
long                    BOOST_TEST_DECL detect_memory_leaks();
int                     BOOST_TEST_DECL random_seed();

} // namespace runtime_config

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: unit_test_parameters.hpp,v $
//  Revision 1.23.2.1  2006/11/13 20:06:57  jhunold
//  Merge from HEAD:
//  Added missing export declarations.
//
//  Revision 1.23  2006/01/30 07:29:49  rogeeff
//  split memory leaks detection API in two to get more functions with better defined roles
//
//  Revision 1.22  2005/12/14 04:58:30  rogeeff
//  new parameter --break_exec_path introduced
//
//  Revision 1.21  2005/04/05 06:11:37  rogeeff
//  memory leak allocation point detection\nextra help with _WIN32_WINNT
//
//  Revision 1.20  2005/02/21 10:18:30  rogeeff
//  random cla support
//
//  Revision 1.19  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_PARAMETERS_HPP_071894GER
