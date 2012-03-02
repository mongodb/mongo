//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 49312 $
//
//  Description : plain report formatter definition
// ***************************************************************************

#ifndef BOOST_TEST_PLAIN_REPORT_FORMATTER_IPP_020105GER
#define BOOST_TEST_PLAIN_REPORT_FORMATTER_IPP_020105GER

// Boost.Test
#include <boost/test/output/plain_report_formatter.hpp>
#include <boost/test/utils/custom_manip.hpp>
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test_suite_impl.hpp>

#include <boost/test/utils/basic_cstring/io.hpp>

// STL
#include <iomanip>
#include <boost/config/no_tr1/cmath.hpp>
#include <iostream>

#include <boost/test/detail/suppress_warnings.hpp>

# ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::log10; }
# endif

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace output {

namespace {

typedef custom_manip<struct quote_t> quote;

template<typename T>
inline std::ostream&
operator<<( custom_printer<quote> const& p, T const& value )
{
    *p << '"' << value << '"';

    return *p;
}

//____________________________________________________________________________//

void
print_stat_value( std::ostream& ostr, counter_t v, counter_t indent, counter_t total,
                  const_string name, const_string res )
{
    if( v > 0 ) {
        ostr << std::setw( indent ) << ""
             << v << ' ' << name << ( v != 1 ? "s" : "" );
        if( total > 0 )
            ostr << " out of " << total;

        ostr << ' ' << res << '\n';
    }
}

//____________________________________________________________________________//

} // local namespace

// ************************************************************************** //
// **************             plain_report_formatter           ************** //
// ************************************************************************** //

void
plain_report_formatter::results_report_start( std::ostream& ostr )
{
    m_indent = 0;
    ostr << '\n';
}

//____________________________________________________________________________//

void
plain_report_formatter::results_report_finish( std::ostream& ostr )
{
    ostr.flush();
}

//____________________________________________________________________________//

void
plain_report_formatter::test_unit_report_start( test_unit const& tu, std::ostream& ostr )
{
    test_results const& tr = results_collector.results( tu.p_id );

    const_string descr;

    if( tr.passed() )
        descr = "passed";
    else if( tr.p_skipped )
        descr = "skipped";
    else if( tr.p_aborted )
        descr = "aborted";
    else
        descr = "failed";

    ostr << std::setw( m_indent ) << ""
         << "Test " << (tu.p_type == tut_case ? "case " : "suite " ) << quote() << tu.p_name << ' ' << descr;

    if( tr.p_skipped ) {
        ostr << " due to " << (tu.check_dependencies() ? "test aborting\n" : "failed dependancy\n" );
        m_indent += 2;
        return;
    }
    
    counter_t total_assertions  = tr.p_assertions_passed + tr.p_assertions_failed;
    counter_t total_tc          = tr.p_test_cases_passed + tr.p_test_cases_failed + tr.p_test_cases_skipped;

    if( total_assertions > 0 || total_tc > 0 )
        ostr << " with:";

    ostr << '\n';
    m_indent += 2;

    print_stat_value( ostr, tr.p_assertions_passed, m_indent, total_assertions, "assertion", "passed" );
    print_stat_value( ostr, tr.p_assertions_failed, m_indent, total_assertions, "assertion", "failed" );
    print_stat_value( ostr, tr.p_expected_failures, m_indent, 0               , "failure"  , "expected" );
    print_stat_value( ostr, tr.p_test_cases_passed, m_indent, total_tc        , "test case", "passed" );
    print_stat_value( ostr, tr.p_test_cases_failed, m_indent, total_tc        , "test case", "failed" );
    print_stat_value( ostr, tr.p_test_cases_skipped, m_indent, total_tc       , "test case", "skipped" );
    print_stat_value( ostr, tr.p_test_cases_aborted, m_indent, total_tc       , "test case", "aborted" );
    
    ostr << '\n';
}

//____________________________________________________________________________//

void
plain_report_formatter::test_unit_report_finish( test_unit const&, std::ostream& )
{
    m_indent -= 2;
}

//____________________________________________________________________________//

void
plain_report_formatter::do_confirmation_report( test_unit const& tu, std::ostream& ostr )
{
    test_results const& tr = results_collector.results( tu.p_id );
    
    if( tr.passed() ) {
        ostr << "*** No errors detected\n";
        return;
    }
        
    if( tr.p_skipped ) {
        ostr << "*** Test " << tu.p_type_name << " skipped due to " 
             << (tu.check_dependencies() ? "test aborting\n" : "failed dependancy\n" );
        return;
    }

    if( tr.p_assertions_failed == 0 ) {
        ostr << "*** errors detected in test " << tu.p_type_name << " " << quote() << tu.p_name
             << "; see standard output for details\n";
        return;
    }

    counter_t num_failures = tr.p_assertions_failed;
    
    ostr << "*** " << num_failures << " failure" << ( num_failures != 1 ? "s" : "" ) << " detected";
    
    if( tr.p_expected_failures > 0 )
        ostr << " (" << tr.p_expected_failures << " failure" << ( tr.p_expected_failures != 1 ? "s" : "" ) << " expected)";
    
    ostr << " in test " << tu.p_type_name << " " << quote() << tu.p_name << "\n";
}

//____________________________________________________________________________//

} // namespace output

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_PLAIN_REPORT_FORMATTER_IPP_020105GER
