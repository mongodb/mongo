//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: test_exec_monitor.hpp,v $
//
//  Version     : $Revision: 1.15 $
//
//  Description : included (vs. linked) version of Test Execution Monitor
// ***************************************************************************

#ifndef BOOST_INCLUDED_TEST_EXEC_MONITOR_HPP_071894GER
#define BOOST_INCLUDED_TEST_EXEC_MONITOR_HPP_071894GER

#include <boost/test/impl/compiler_log_formatter.ipp>
#include <boost/test/impl/execution_monitor.ipp>
#include <boost/test/impl/framework.ipp>
#include <boost/test/impl/plain_report_formatter.ipp>
#include <boost/test/impl/progress_monitor.ipp>
#include <boost/test/impl/results_collector.ipp>
#include <boost/test/impl/results_reporter.ipp>
#include <boost/test/impl/test_main.ipp>
#include <boost/test/impl/test_tools.ipp>
#include <boost/test/impl/unit_test_log.ipp>
#include <boost/test/impl/unit_test_main.ipp>
#include <boost/test/impl/unit_test_monitor.ipp>
#include <boost/test/impl/unit_test_parameters.ipp>
#include <boost/test/impl/unit_test_suite.ipp>
#include <boost/test/impl/xml_log_formatter.ipp>
#include <boost/test/impl/xml_report_formatter.ipp>

#define BOOST_TEST_INCLUDED
#include <boost/test/test_exec_monitor.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: test_exec_monitor.hpp,v $
//  Revision 1.15  2006/02/06 10:01:55  rogeeff
//  m,ake name similar to the primary header name
//
//  Revision 1.14  2006/02/01 07:57:49  rogeeff
//  included components entry points
//
//  Revision 1.13  2005/02/20 08:27:08  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.12  2005/02/01 08:59:38  rogeeff
//  supplied_log_formatters split
//  change formatters interface to simplify result interface
//
//  Revision 1.11  2005/02/01 06:40:07  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.10  2005/01/22 19:22:13  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
// ***************************************************************************

#endif // BOOST_INCLUDED_TEST_EXEC_MONITOR_HPP_071894GER
