//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: log_level.hpp,v $
//
//  Version     : $Revision: 1.2 $
//
//  Description : shared definition for unit test log levels
// ***************************************************************************

#ifndef BOOST_TEST_LOG_LEVEL_HPP_011605GER
#define BOOST_TEST_LOG_LEVEL_HPP_011605GER

namespace boost {
namespace unit_test {

// ************************************************************************** //
// **************                   log levels                 ************** //
// ************************************************************************** //

//  each log level includes all subsequent higher loging levels
enum            log_level {
    invalid_log_level        = -1,
    log_successful_tests     = 0,
    log_test_suites          = 1,
    log_messages             = 2,
    log_warnings             = 3,
    log_all_errors           = 4, // reported by unit test macros
    log_cpp_exception_errors = 5, // uncaught C++ exceptions
    log_system_errors        = 6, // including timeouts, signals, traps
    log_fatal_errors         = 7, // including unit test macros or
                                  // fatal system errors
    log_nothing              = 8
};

} // namespace unit_test
} // namespace boost

// ***************************************************************************
//  Revision History :
//  
//  $Log: log_level.hpp,v $
//  Revision 1.2  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.1  2005/01/18 08:27:30  rogeeff
//  unit_test_log rework:
//     eliminated need for ::instance()
//     eliminated need for << end and ...END macro
//     straitend interface between log and formatters
//     change compiler like formatter name
//     minimized unit_test_log interface and reworked to use explicit calls
//
// ***************************************************************************

#endif // BOOST_TEST_LOG_LEVEL_HPP_011605GER
