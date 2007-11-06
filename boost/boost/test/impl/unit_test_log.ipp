//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test_log.ipp,v $
//
//  Version     : $Revision: 1.11 $
//
//  Description : implemets Unit Test Log
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_LOG_IPP_012205GER
#define BOOST_TEST_UNIT_TEST_LOG_IPP_012205GER

// Boost.Test
#include <boost/test/unit_test_log.hpp>
#include <boost/test/unit_test_log_formatter.hpp>
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/execution_monitor.hpp>

#include <boost/test/detail/unit_test_parameters.hpp>
#include <boost/test/detail/wrap_io_saver.hpp>

#include <boost/test/utils/basic_cstring/compare.hpp>

#include <boost/test/output/compiler_log_formatter.hpp>
#include <boost/test/output/xml_log_formatter.hpp>

// Boost
#include <boost/scoped_ptr.hpp>

// STL
#include <iostream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************             entry_value_collector            ************** //
// ************************************************************************** //

namespace ut_detail {

entry_value_collector
entry_value_collector::operator<<( const_string v )
{
    unit_test_log << v;

    m_last = false;

    entry_value_collector res;
    return res;
}

//____________________________________________________________________________//

entry_value_collector::~entry_value_collector()
{
    if( m_last )
        unit_test_log << log::end();
}

//____________________________________________________________________________//

} // namespace ut_detail

// ************************************************************************** //
// **************                 unit_test_log                ************** //
// ************************************************************************** //

namespace {

struct unit_test_log_impl {
    // Constructor
    unit_test_log_impl()
    : m_stream( &std::cout )
    , m_stream_state_saver( new io_saver_type( std::cout ) )
    , m_threshold_level( log_all_errors )
    , m_log_formatter( new output::compiler_log_formatter )
    {
    }

    // log data
    typedef scoped_ptr<unit_test_log_formatter> formatter_ptr;
    typedef scoped_ptr<io_saver_type>           saver_ptr;

    std::ostream*       m_stream;
    saver_ptr           m_stream_state_saver;
    log_level           m_threshold_level;
    formatter_ptr       m_log_formatter;

    // entry data
    bool                m_entry_in_progress;
    bool                m_entry_started;
    log_entry_data      m_entry_data;

    // check point data
    log_checkpoint_data m_checkpoint_data;

    // helper functions
    std::ostream&       stream()            { return *m_stream; }
    void                set_checkpoint( const_string file, std::size_t line_num, const_string msg )
    {
        assign_op( m_checkpoint_data.m_message, msg, 0 );
        m_checkpoint_data.m_file_name    = file;
        m_checkpoint_data.m_line_num    = line_num;
    }
};

unit_test_log_impl& s_log_impl() { static unit_test_log_impl the_inst; return the_inst; }

} // local namespace

//____________________________________________________________________________//

void
unit_test_log_t::test_start( counter_t test_cases_amount )
{
    s_log_impl().m_log_formatter->log_start( s_log_impl().stream(), test_cases_amount );

    if( runtime_config::show_build_info() )
        s_log_impl().m_log_formatter->log_build_info( s_log_impl().stream() );

    s_log_impl().m_entry_in_progress = false;
}

//____________________________________________________________________________//

void
unit_test_log_t::test_finish()
{
    s_log_impl().m_log_formatter->log_finish( s_log_impl().stream() );
}

//____________________________________________________________________________//

void
unit_test_log_t::test_aborted()
{
    BOOST_TEST_LOG_ENTRY( log_messages ) << "Test is aborted";
}

//____________________________________________________________________________//

void
unit_test_log_t::test_unit_start( test_unit const& tu )
{
    if( s_log_impl().m_threshold_level > log_test_suites )
        return;

    if( s_log_impl().m_entry_in_progress )
        *this << log::end();

    s_log_impl().m_log_formatter->test_unit_start( s_log_impl().stream(), tu );
}

//____________________________________________________________________________//

void
unit_test_log_t::test_unit_finish( test_unit const& tu, unsigned long elapsed )
{
    if( s_log_impl().m_threshold_level > log_test_suites )
        return;

    s_log_impl().m_checkpoint_data.clear();

    if( s_log_impl().m_entry_in_progress )
        *this << log::end();

    s_log_impl().m_log_formatter->test_unit_finish( s_log_impl().stream(), tu, elapsed );
}

//____________________________________________________________________________//

void
unit_test_log_t::test_unit_skipped( test_unit const& tu )
{
    if( s_log_impl().m_threshold_level > log_test_suites )
        return;

    if( s_log_impl().m_entry_in_progress )
        *this << log::end();

    s_log_impl().m_log_formatter->test_unit_skipped( s_log_impl().stream(), tu );
}

//____________________________________________________________________________//

void
unit_test_log_t::test_unit_aborted( test_unit const& )
{
    // do nothing
}

//____________________________________________________________________________//

void
unit_test_log_t::assertion_result( bool )
{
    // do nothing
}

//____________________________________________________________________________//

void
unit_test_log_t::exception_caught( execution_exception const& ex )
{
    log_level l =
        ex.code() <= execution_exception::cpp_exception_error   ? log_cpp_exception_errors :
        (ex.code() <= execution_exception::timeout_error        ? log_system_errors
                                                                : log_fatal_errors );

    if( l >= s_log_impl().m_threshold_level ) {
        if( s_log_impl().m_entry_in_progress )
            *this << log::end();

        s_log_impl().m_log_formatter->log_exception( s_log_impl().stream(), s_log_impl().m_checkpoint_data, ex.what() );
    }
}

//____________________________________________________________________________//

void
unit_test_log_t::set_checkpoint( const_string file, std::size_t line_num, const_string msg )
{
    s_log_impl().set_checkpoint( file, line_num, msg );
}

//____________________________________________________________________________//

char
set_unix_slash( char in )
{
    return in == '\\' ? '/' : in;
}
    
unit_test_log_t&
unit_test_log_t::operator<<( log::begin const& b )
{
    if( s_log_impl().m_entry_in_progress )
        *this << log::end();

    s_log_impl().m_stream_state_saver->restore();

    s_log_impl().m_entry_data.clear();

    assign_op( s_log_impl().m_entry_data.m_file_name, b.m_file_name, 0 );

    // normalize file name
    std::transform( s_log_impl().m_entry_data.m_file_name.begin(), s_log_impl().m_entry_data.m_file_name.end(),
                    s_log_impl().m_entry_data.m_file_name.begin(),
                    &set_unix_slash );

    s_log_impl().m_entry_data.m_line_num = b.m_line_num;

    return *this;
}

//____________________________________________________________________________//

unit_test_log_t&
unit_test_log_t::operator<<( log::end const& )
{
    if( s_log_impl().m_entry_in_progress )
        s_log_impl().m_log_formatter->log_entry_finish( s_log_impl().stream() );

    s_log_impl().m_entry_in_progress = false;

    return *this;
}

//____________________________________________________________________________//

unit_test_log_t&
unit_test_log_t::operator<<( log_level l )
{
    s_log_impl().m_entry_data.m_level = l;

    return *this;
}

//____________________________________________________________________________//

ut_detail::entry_value_collector
unit_test_log_t::operator()( log_level l )
{
    *this << l;

    ut_detail::entry_value_collector res;
    return res;
}

//____________________________________________________________________________//

unit_test_log_t&
unit_test_log_t::operator<<( const_string value )
{
    if( s_log_impl().m_entry_data.m_level >= s_log_impl().m_threshold_level && !value.empty() ) {
        if( !s_log_impl().m_entry_in_progress ) {
            s_log_impl().m_entry_in_progress = true;

            switch( s_log_impl().m_entry_data.m_level ) {
            case log_successful_tests:
                s_log_impl().m_log_formatter->log_entry_start( s_log_impl().stream(), s_log_impl().m_entry_data,
                                                               unit_test_log_formatter::BOOST_UTL_ET_INFO );
                break;
            case log_messages:
                s_log_impl().m_log_formatter->log_entry_start( s_log_impl().stream(), s_log_impl().m_entry_data,
                                                               unit_test_log_formatter::BOOST_UTL_ET_MESSAGE );
                break;
            case log_warnings:
                s_log_impl().m_log_formatter->log_entry_start( s_log_impl().stream(), s_log_impl().m_entry_data,
                                                               unit_test_log_formatter::BOOST_UTL_ET_WARNING );
                break;
            case log_all_errors:
            case log_cpp_exception_errors:
            case log_system_errors:
                s_log_impl().m_log_formatter->log_entry_start( s_log_impl().stream(), s_log_impl().m_entry_data,
                                                               unit_test_log_formatter::BOOST_UTL_ET_ERROR );
                break;
            case log_fatal_errors:
                s_log_impl().m_log_formatter->log_entry_start( s_log_impl().stream(), s_log_impl().m_entry_data,
                                                               unit_test_log_formatter::BOOST_UTL_ET_FATAL_ERROR );
                break;
            case log_nothing:
            case log_test_suites:
            case invalid_log_level:
                return *this;
            }
        }

        s_log_impl().m_log_formatter->log_entry_value( s_log_impl().stream(), value );
    }

    return *this;
}

//____________________________________________________________________________//

void
unit_test_log_t::set_stream( std::ostream& str )
{
    if( s_log_impl().m_entry_in_progress )
        return;

    s_log_impl().m_stream = &str;
    s_log_impl().m_stream_state_saver.reset( new io_saver_type( str ) );
}

//____________________________________________________________________________//

void
unit_test_log_t::set_threshold_level( log_level lev )
{
    if( s_log_impl().m_entry_in_progress || lev == invalid_log_level )
        return;

    s_log_impl().m_threshold_level = lev;
}

//____________________________________________________________________________//

void
unit_test_log_t::set_format( output_format log_format )
{
    if( s_log_impl().m_entry_in_progress )
        return;

    if( log_format == CLF )
        set_formatter( new output::compiler_log_formatter );
    else
        set_formatter( new output::xml_log_formatter );
}

//____________________________________________________________________________//

void
unit_test_log_t::set_formatter( unit_test_log_formatter* the_formatter )
{
    s_log_impl().m_log_formatter.reset( the_formatter );
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: unit_test_log.ipp,v $
//  Revision 1.11  2005/12/14 05:34:21  rogeeff
//  log API simplified
//
//  Revision 1.10  2005/04/30 16:48:51  rogeeff
//  io saver warkaround for classic io is shared
//
//  Revision 1.9  2005/04/29 06:28:35  rogeeff
//  bug fix for manipulator handling
//
//  Revision 1.8  2005/04/12 06:50:46  rogeeff
//  assign_to -> assign_op
//
//  Revision 1.7  2005/03/22 07:06:58  rogeeff
//  assign_to made free function
//
//  Revision 1.6  2005/02/20 08:27:07  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_LOG_IPP_012205GER
