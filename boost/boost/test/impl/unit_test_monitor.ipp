//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test_monitor.ipp,v $
//
//  Version     : $Revision: 1.4 $
//
//  Description : implements specific subclass of Executon Monitor used by Unit
//  Test Framework to monitor test cases run.
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_MONITOR_IPP_012205GER
#define BOOST_TEST_UNIT_TEST_MONITOR_IPP_012205GER

// Boost.Test
#include <boost/test/unit_test_monitor.hpp>
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/test_tools.hpp>
#include <boost/test/framework.hpp>

#include <boost/test/detail/unit_test_parameters.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace {

template<typename F>
struct zero_return_wrapper_t {
    explicit zero_return_wrapper_t( F const& f ) : m_f( f ) {}
    
    int operator()() { m_f(); return 0; }
    
    F const& m_f;
};

template<typename F>
zero_return_wrapper_t<F>
zero_return_wrapper( F const& f )
{
    return zero_return_wrapper_t<F>( f );
}

}

// ************************************************************************** //
// **************               unit_test_monitor              ************** //
// ************************************************************************** //

unit_test_monitor_t::error_level
unit_test_monitor_t::execute_and_translate( test_case const& tc )
{
    try {
        execute( callback0<int>( zero_return_wrapper( tc.test_func() ) ),
                 runtime_config::catch_sys_errors(),
                 tc.p_timeout );
    }
    catch( execution_exception const& ex ) {
        framework::exception_caught( ex );
        framework::test_unit_aborted( framework::current_test_case() );

        // translate execution_exception::error_code to error_level
        switch( ex.code() ) {
        case execution_exception::no_error:             return test_ok;
        case execution_exception::user_error:           return unexpected_exception;
        case execution_exception::cpp_exception_error:  return unexpected_exception;
        case execution_exception::system_error:         return os_exception;
        case execution_exception::timeout_error:        return os_timeout;
        case execution_exception::user_fatal_error:
        case execution_exception::system_fatal_error:   return fatal_error;
        default:                                        return unexpected_exception;
        }
    }

    return test_ok;
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: unit_test_monitor.ipp,v $
//  Revision 1.4  2005/12/14 05:37:52  rogeeff
//  zero_return_wrapper made to template
//  comply to test_unit_aborted() modification
//
//  Revision 1.3  2005/02/20 08:27:07  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.2  2005/02/01 06:40:07  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.1  2005/01/22 19:22:13  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
//  Revision 1.17  2005/01/19 16:34:07  vawjr
//  Changed the \r\r\n back to \r\n on windows so we don't get errors when compiling
//  on VC++8.0.  I don't know why Microsoft thinks it's a good idea to call this an error,
//  but they do.  I also don't know why people insist on checking out files on Windows and
//  copying them to a unix system to check them in (which will cause exactly this problem)
//
//  Revision 1.16  2005/01/18 08:30:08  rogeeff
//  unit_test_log rework:
//     eliminated need for ::instance()
//     eliminated need for << end and ...END macro
//     straitend interface between log and formatters
//     change compiler like formatter name
//     minimized unit_test_log interface and reworked to use explicit calls
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_MONITOR_IPP_012205GER
