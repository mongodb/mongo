//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test.hpp,v $
//
//  Version     : $Revision: 1.19 $
//
//  Description : Entry point for the end user into the Unit Test Framework.
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_HPP_071894GER
#define BOOST_TEST_UNIT_TEST_HPP_071894GER

// Boost.Test
#include <boost/test/test_tools.hpp>
#include <boost/test/unit_test_suite.hpp>

//____________________________________________________________________________//

// ************************************************************************** //
// **************                 Auto Linking                 ************** //
// ************************************************************************** //

#if !defined(BOOST_ALL_NO_LIB) && !defined(BOOST_TEST_NO_LIB) && \
    !defined(BOOST_TEST_SOURCE) && !defined(BOOST_TEST_INCLUDED)
#  define BOOST_LIB_NAME boost_unit_test_framework

#  if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_TEST_DYN_LINK)
#    define BOOST_DYN_LINK
#  endif

#  include <boost/config/auto_link.hpp>

#endif  // auto-linking disabled

// ************************************************************************** //
// **************                  unit_test_main              ************** //
// ************************************************************************** //

namespace boost { namespace unit_test {

#if defined(BOOST_TEST_DYN_LINK) 

int BOOST_TEST_DECL unit_test_main( bool (*init_unit_test_func)(), int argc, char* argv[] );

#else

int BOOST_TEST_DECL unit_test_main( int argc, char* argv[] );

#endif

}}

#if defined(BOOST_TEST_DYN_LINK) && defined(BOOST_TEST_MAIN) && !defined(BOOST_TEST_NO_MAIN)

// ************************************************************************** //
// **************        main function for tests using dll     ************** //
// ************************************************************************** //

int BOOST_TEST_CALL_DECL
main( int argc, char* argv[] )
{
    return ::boost::unit_test::unit_test_main( &init_unit_test, argc, argv );
}

//____________________________________________________________________________//

#endif // BOOST_TEST_DYN_LINK && BOOST_TEST_MAIN && !BOOST_TEST_NO_MAIN

// ***************************************************************************
//  Revision History :
//  
//  $Log: unit_test.hpp,v $
//  Revision 1.19  2006/03/19 11:45:26  rogeeff
//  main function renamed for consistancy
//
//  Revision 1.18  2006/02/07 16:15:20  rogeeff
//  BOOST_TEST_INCLUDED guard were missing
//
//  Revision 1.17  2006/02/06 10:04:55  rogeeff
//  BOOST_TEST_MODULE - master test suite name
//
//  Revision 1.16  2005/12/14 05:21:36  rogeeff
//  dll support introduced
//  auto linking support introduced
//
//  Revision 1.15  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.14  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_HPP_071894GER
