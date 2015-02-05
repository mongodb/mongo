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
//  Description : implements compiler like Log formatter
// ***************************************************************************

#ifndef BOOST_TEST_COMPILER_LOG_FORMATTER_IPP_020105GER
#define BOOST_TEST_COMPILER_LOG_FORMATTER_IPP_020105GER

// Boost.Test
#include <boost/test/output/compiler_log_formatter.hpp>
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/framework.hpp>
#include <boost/test/utils/basic_cstring/io.hpp>
#include <boost/test/utils/lazy_ostream.hpp>

// Boost
#include <boost/version.hpp>

// STL
#include <iostream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace output {

// ************************************************************************** //
// **************            compiler_log_formatter            ************** //
// ************************************************************************** //

namespace {

const_string
test_phase_identifier()
{
    return framework::is_initialized() 
            ? const_string( framework::current_test_case().p_name.get() )
            : BOOST_TEST_L( "Test setup" );
}

} // local namespace

//____________________________________________________________________________//

void
compiler_log_formatter::log_start( std::ostream& output, counter_t test_cases_amount )
{
    if( test_cases_amount > 0 )
        output  << "Running " << test_cases_amount << " test "
                << (test_cases_amount > 1 ? "cases" : "case") << "...\n";
}

//____________________________________________________________________________//

void
compiler_log_formatter::log_finish( std::ostream& ostr )
{
    ostr.flush();
}

//____________________________________________________________________________//

void
compiler_log_formatter::log_build_info( std::ostream& output )
{
    output  << "Platform: " << BOOST_PLATFORM            << '\n'
            << "Compiler: " << BOOST_COMPILER            << '\n'
            << "STL     : " << BOOST_STDLIB              << '\n'
            << "Boost   : " << BOOST_VERSION/100000      << "."
                            << BOOST_VERSION/100 % 1000  << "."
                            << BOOST_VERSION % 100       << std::endl;
}

//____________________________________________________________________________//

void
compiler_log_formatter::test_unit_start( std::ostream& output, test_unit const& tu )
{
    output << "Entering test " << tu.p_type_name << " \"" << tu.p_name << "\"" << std::endl;
}

//____________________________________________________________________________//

void
compiler_log_formatter::test_unit_finish( std::ostream& output, test_unit const& tu, unsigned long elapsed )
{
    output << "Leaving test " << tu.p_type_name << " \"" << tu.p_name << "\"";

    if( elapsed > 0 ) {
        output << "; testing time: ";
        if( elapsed % 1000 == 0 )
            output << elapsed/1000 << "ms";
        else
            output << elapsed << "mks";
    }

    output << std::endl;
}

//____________________________________________________________________________//

void
compiler_log_formatter::test_unit_skipped( std::ostream& output, test_unit const& tu )
{
    output  << "Test " << tu.p_type_name << " \"" << tu.p_name << "\"" << "is skipped" << std::endl;
}
    
//____________________________________________________________________________//

void
compiler_log_formatter::log_exception( std::ostream& output, log_checkpoint_data const& checkpoint_data, execution_exception const& ex )
{
    execution_exception::location const& loc = ex.where();
    print_prefix( output, loc.m_file_name, loc.m_line_num );

    output << "fatal error in \"" << (loc.m_function.is_empty() ? test_phase_identifier() : loc.m_function ) << "\": ";

    output << ex.what();

    if( !checkpoint_data.m_file_name.is_empty() ) {
        output << '\n';
        print_prefix( output, checkpoint_data.m_file_name, checkpoint_data.m_line_num );
        output << "last checkpoint";
        if( !checkpoint_data.m_message.empty() )
            output << ": " << checkpoint_data.m_message;
    }
    
    output << std::endl;
}

//____________________________________________________________________________//

void
compiler_log_formatter::log_entry_start( std::ostream& output, log_entry_data const& entry_data, log_entry_types let )
{
    switch( let ) {
        case BOOST_UTL_ET_INFO:
            print_prefix( output, entry_data.m_file_name, entry_data.m_line_num );
            output << "info: ";
            break;
        case BOOST_UTL_ET_MESSAGE:
            break;
        case BOOST_UTL_ET_WARNING:
            print_prefix( output, entry_data.m_file_name, entry_data.m_line_num );
            output << "warning in \"" << test_phase_identifier() << "\": ";
            break;
        case BOOST_UTL_ET_ERROR:
            print_prefix( output, entry_data.m_file_name, entry_data.m_line_num );
            output << "error in \"" << test_phase_identifier() << "\": ";
            break;
        case BOOST_UTL_ET_FATAL_ERROR:
            print_prefix( output, entry_data.m_file_name, entry_data.m_line_num );
            output << "fatal error in \"" << test_phase_identifier() << "\": ";
            break;
    }
}

//____________________________________________________________________________//

void
compiler_log_formatter::log_entry_value( std::ostream& output, const_string value )
{
    output << value;
}

//____________________________________________________________________________//

void
compiler_log_formatter::log_entry_value( std::ostream& output, lazy_ostream const& value )
{
    output << value;
}

//____________________________________________________________________________//

void
compiler_log_formatter::log_entry_finish( std::ostream& output )
{
    output << std::endl;
}

//____________________________________________________________________________//

void
compiler_log_formatter::print_prefix( std::ostream& output, const_string file, std::size_t line )
{
#ifdef __APPLE_CC__
    // Xcode-compatible logging format, idea by Richard Dingwall at 
    // <http://richarddingwall.name/2008/06/01/using-the-boost-unit-test-framework-with-xcode-3/>. 
    output << file << ':' << line << ": ";
#else
    output << file << '(' << line << "): ";
#endif
}

//____________________________________________________________________________//

} // namespace output

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_COMPILER_LOG_FORMATTER_IPP_020105GER
