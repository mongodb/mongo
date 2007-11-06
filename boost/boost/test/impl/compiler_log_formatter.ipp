//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: compiler_log_formatter.ipp,v $
//
//  Version     : $Revision: 1.4 $
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

void
compiler_log_formatter::log_start( std::ostream& output, counter_t test_cases_amount )
{
    if( test_cases_amount > 0 )
        output  << "Running " << test_cases_amount << " test "
                << (test_cases_amount > 1 ? "cases" : "case") << "...\n";
}

//____________________________________________________________________________//

void
compiler_log_formatter::log_finish( std::ostream& )
{
    // do nothing
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
compiler_log_formatter::log_exception( std::ostream& output, log_checkpoint_data const& checkpoint_data, const_string explanation )
{
    print_prefix( output, BOOST_TEST_L( "unknown location" ), 0 );
    output << "fatal error in \"" << framework::current_test_case().p_name << "\": ";

    if( !explanation.is_empty() )
        output << explanation;
    else
        output << "uncaught exception, system error or abort requested";


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
            output << "warning in \"" << framework::current_test_case().p_name << "\": ";
            break;
        case BOOST_UTL_ET_ERROR:
            print_prefix( output, entry_data.m_file_name, entry_data.m_line_num );
            output << "error in \"" << framework::current_test_case().p_name << "\": ";
            break;
        case BOOST_UTL_ET_FATAL_ERROR:
            print_prefix( output, entry_data.m_file_name, entry_data.m_line_num );
            output << "fatal error in \"" << framework::current_test_case().p_name << "\": ";
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
compiler_log_formatter::log_entry_finish( std::ostream& output )
{
    output << std::endl;
}

//____________________________________________________________________________//

void
compiler_log_formatter::print_prefix( std::ostream& output, const_string file, std::size_t line )
{
    output << file << '(' << line << "): ";
}

//____________________________________________________________________________//

} // namespace ouptut

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: compiler_log_formatter.ipp,v $
//  Revision 1.4  2005/12/14 05:26:32  rogeeff
//  report aborted test units
//
//  Revision 1.3  2005/02/21 10:09:26  rogeeff
//  exception logging changes so that it produce a string recognizable by compiler as an error
//
//  Revision 1.2  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.1  2005/02/01 08:59:38  rogeeff
//  supplied_log_formatters split
//  change formatters interface to simplify result interface
//
// ***************************************************************************

#endif // BOOST_TEST_COMPILER_LOG_FORMATTER_IPP_020105GER
