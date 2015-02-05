//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
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

#include <boost/test/utils/basic_cstring/compare.hpp>

#include <boost/test/output/compiler_log_formatter.hpp>
#include <boost/test/output/xml_log_formatter.hpp>

// Boost
#include <boost/scoped_ptr.hpp>
#include <boost/io/ios_state.hpp>
typedef ::boost::io::ios_base_all_saver io_saver_type;

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************             entry_value_collector            ************** //
// ************************************************************************** //

namespace ut_detail {

entry_value_collector const&
entry_value_collector::operator<<( lazy_ostream const& v ) const
{
    unit_test_log << v;

    return *this;
}

//____________________________________________________________________________//

entry_value_collector const& 
entry_value_collector::operator<<( const_string v ) const
{
    unit_test_log << v;

    return *this;
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
    : m_stream( runtime_config::log_sink() )
    , m_stream_state_saver( new io_saver_type( *m_stream ) )
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
    if( s_log_impl().m_threshold_level == log_nothing )
        return;

    s_log_impl().m_log_formatter->log_start( s_log_impl().stream(), test_cases_amount );

    if( runtime_config::show_build_info() )
        s_log_impl().m_log_formatter->log_build_info( s_log_impl().stream() );

    s_log_impl().m_entry_in_progress = false;
}

//____________________________________________________________________________//

void
unit_test_log_t::test_finish()
{
    if( s_log_impl().m_threshold_level == log_nothing )
        return;

    s_log_impl().m_log_formatter->log_finish( s_log_impl().stream() );

    s_log_impl().stream().flush();
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
    if( s_log_impl().m_threshold_level > log_test_units )
        return;

    if( s_log_impl().m_entry_in_progress )
        *this << log::end();

    s_log_impl().m_log_formatter->test_unit_start( s_log_impl().stream(), tu );
}

//____________________________________________________________________________//

void
unit_test_log_t::test_unit_finish( test_unit const& tu, unsigned long elapsed )
{
    if( s_log_impl().m_threshold_level > log_test_units )
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
    if( s_log_impl().m_threshold_level > log_test_units )
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

        s_log_impl().m_log_formatter->log_exception( s_log_impl().stream(), s_log_impl().m_checkpoint_data, ex );
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

    return ut_detail::entry_value_collector();
}

//____________________________________________________________________________//

bool
unit_test_log_t::log_entry_start()
{
    if( s_log_impl().m_entry_in_progress ) 
        return true;

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
    case log_test_units:
    case invalid_log_level:
        return false;
    }

    s_log_impl().m_entry_in_progress = true;

    return true;
}

//____________________________________________________________________________//

unit_test_log_t&
unit_test_log_t::operator<<( const_string value )
{
    if( s_log_impl().m_entry_data.m_level >= s_log_impl().m_threshold_level && !value.empty() && log_entry_start() )
        s_log_impl().m_log_formatter->log_entry_value( s_log_impl().stream(), value );

    return *this;
}

//____________________________________________________________________________//

unit_test_log_t&
unit_test_log_t::operator<<( lazy_ostream const& value )
{
    if( s_log_impl().m_entry_data.m_level >= s_log_impl().m_threshold_level && !value.empty() && log_entry_start() )
        s_log_impl().m_log_formatter->log_entry_value( s_log_impl().stream(), value );

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

// ************************************************************************** //
// **************            unit_test_log_formatter           ************** //
// ************************************************************************** //

void
unit_test_log_formatter::log_entry_value( std::ostream& ostr, lazy_ostream const& value )
{
    log_entry_value( ostr, (wrap_stringstream().ref() << value).str() );
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_UNIT_TEST_LOG_IPP_012205GER
