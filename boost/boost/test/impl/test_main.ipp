//  (C) Copyright Gennadiy Rozental 2001-2005.
//  (C) Copyright Beman Dawes 1995-2001.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: test_main.ipp,v $
//
//  Version     : $$Revision: 1.8 $
//
//  Description : implements main function for Test Execution Monitor.
// ***************************************************************************

#ifndef BOOST_TEST_TEST_MAIN_IPP_012205GER
#define BOOST_TEST_TEST_MAIN_IPP_012205GER

// Boost.Test
#include <boost/test/framework.hpp>
#include <boost/test/test_tools.hpp>
#include <boost/test/unit_test_suite.hpp>

// Boost
#include <boost/cstdlib.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

extern int test_main( int argc, char* argv[] );    // prototype for user's test_main()

struct test_main_caller {
    test_main_caller( int argc, char** argv ) : m_argc( argc ), m_argv( argv ) {}
    
    void operator()() {
        int test_main_result = test_main( m_argc, m_argv );

        // translate a test_main non-success return into a test error
        BOOST_CHECK( test_main_result == 0 || test_main_result == boost::exit_success );
    }
  
private:
    // Data members    
    int      m_argc;
    char**   m_argv;
};

// ************************************************************************** //
// **************                   test main                  ************** //
// ************************************************************************** //

::boost::unit_test::test_suite*
init_unit_test_suite( int argc, char* argv[] ) {
    using namespace ::boost::unit_test;
    
    framework::master_test_suite().p_name.value = "Test Program";
    
    framework::master_test_suite().add( BOOST_TEST_CASE( test_main_caller( argc, argv ) ) );
    
    return 0;
}

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: test_main.ipp,v $
//  Revision 1.8  2005/12/20 23:50:13  rogeeff
//  unit_test.hpp removed
//
//  Revision 1.7  2005/12/14 05:54:17  rogeeff
//  change existent init API usage
//
//  Revision 1.6  2005/02/20 08:27:07  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_TEST_MAIN_IPP_012205GER
