//  (C) Copyright Gennadiy Rozental 2001-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
//
//  Description : main function implementation for Unit Test Framework
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_MAIN_IPP_012205GER
#define BOOST_TEST_UNIT_TEST_MAIN_IPP_012205GER

// Boost.Test
#include <boost/test/framework.hpp>
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/results_reporter.hpp>

#include <boost/test/detail/unit_test_parameters.hpp>

#if !defined(__BORLANDC__) && !BOOST_WORKAROUND( BOOST_MSVC, < 1300 ) && !BOOST_WORKAROUND( __SUNPRO_CC, < 0x5100 )
#define BOOST_TEST_SUPPORT_RUN_BY_NAME
#include <boost/test/utils/iterator/token_iterator.hpp>
#endif

// Boost
#include <boost/cstdlib.hpp>
#include <boost/bind.hpp>

// STL
#include <stdexcept>
#include <iostream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                 test_case_filter             ************** //
// ************************************************************************** //

class test_case_filter : public test_tree_visitor {
public:
    struct single_filter {
        single_filter( const_string in )
        {
            if( in == "*" )
                m_kind  = SFK_ALL;
            else if( first_char( in ) == '*' && last_char( in ) == '*' ) {
                m_kind  = SFK_SUBSTR;
                m_value = in.substr( 1, in.size()-1 );
            }
            else if( first_char( in ) == '*' ) {
                m_kind  = SFK_TRAILING;
                m_value = in.substr( 1 );
            }
            else if( last_char( in ) == '*' ) {
                m_kind  = SFK_LEADING;
                m_value = in.substr( 0, in.size()-1 );
            }
            else {
                m_kind  = SFK_MATCH;
                m_value = in;
            }
        };

        bool            pass( test_unit const& tu ) const
        {
            const_string name( tu.p_name );
    
            switch( m_kind ) {
            default:
            case SFK_ALL:
                return true;

            case SFK_LEADING:
                return name.substr( 0, m_value.size() ) == m_value;

            case SFK_TRAILING:
                return name.size() >= m_value.size() && name.substr( name.size() - m_value.size() ) == m_value;

            case SFK_SUBSTR:
                return name.find( m_value ) != const_string::npos;

            case SFK_MATCH:
                return m_value == tu.p_name.get();
            }
        }
        enum kind { SFK_ALL, SFK_LEADING, SFK_TRAILING, SFK_SUBSTR, SFK_MATCH };

        kind            m_kind;
        const_string    m_value;
    };
    // Constructor
#ifndef BOOST_TEST_SUPPORT_RUN_BY_NAME
    explicit        test_case_filter( const_string ) : m_depth( 0 ) {}
#else
    explicit        test_case_filter( const_string tc_to_run ) 
    : m_depth( 0 )
    {
        string_token_iterator tit( tc_to_run, (dropped_delimeters = "/", kept_delimeters = dt_none) );

        while( tit != string_token_iterator() ) {
            m_filters.push_back( 
                std::vector<single_filter>( string_token_iterator( *tit, (dropped_delimeters = ",", kept_delimeters = dt_none)  ), 
                                            string_token_iterator() ) );

            ++tit;           
        }
    }
#endif
    
    void            filter_unit( test_unit const& tu )
    {
        if( (++m_depth - 1) > m_filters.size() ) {
            tu.p_enabled.value = true;
            return;
        }

        if( m_depth == 1 )
            return;

        std::vector<single_filter> const& filters = m_filters[m_depth-2];

        tu.p_enabled.value =
            std::find_if( filters.begin(), filters.end(), bind( &single_filter::pass, _1, boost::ref(tu) ) ) != filters.end();
    }

    // test tree visitor interface
    virtual void    visit( test_case const& tc )
    {
        if( m_depth < m_filters.size() ) {
            tc.p_enabled.value = false;
            return;
        }

        filter_unit( tc );

        --m_depth;
    }

    virtual bool    test_suite_start( test_suite const& ts )
    { 
        filter_unit( ts );

        if( !ts.p_enabled )
            --m_depth;

        return ts.p_enabled;
    }

    virtual void    test_suite_finish( test_suite const& )  { --m_depth; }

private:
    // Data members
    std::vector<std::vector<single_filter> >    m_filters;
    unsigned                                    m_depth;
};

// ************************************************************************** //
// **************                  unit_test_main              ************** //
// ************************************************************************** //

int BOOST_TEST_DECL
unit_test_main( init_unit_test_func init_func, int argc, char* argv[] )
{
    try {
        framework::init( init_func, argc, argv );

        if( !runtime_config::test_to_run().is_empty() ) {
            test_case_filter filter( runtime_config::test_to_run() );

            traverse_test_tree( framework::master_test_suite().p_id, filter );
        }

        framework::run();

        results_reporter::make_report();

        return runtime_config::no_result_code() 
                    ? boost::exit_success
                    : results_collector.results( framework::master_test_suite().p_id ).result_code();
    }
    catch( framework::nothing_to_test const& ) {
        return boost::exit_success;
    }
    catch( framework::internal_error const& ex ) {
        results_reporter::get_stream() << "Boost.Test framework internal error: " << ex.what() << std::endl;
        
        return boost::exit_exception_failure;
    }
    catch( framework::setup_error const& ex ) {
        results_reporter::get_stream() << "Test setup error: " << ex.what() << std::endl;
        
        return boost::exit_exception_failure;
    }
    catch( ... ) {
        results_reporter::get_stream() << "Boost.Test framework internal error: unknown reason" << std::endl;
        
        return boost::exit_exception_failure;
    }
}

} // namespace unit_test

} // namespace boost

#if !defined(BOOST_TEST_DYN_LINK) && !defined(BOOST_TEST_NO_MAIN)

// ************************************************************************** //
// **************        main function for tests using lib     ************** //
// ************************************************************************** //

int BOOST_TEST_CALL_DECL
main( int argc, char* argv[] )
{
    // prototype for user's unit test init function
#ifdef BOOST_TEST_ALTERNATIVE_INIT_API
    extern bool init_unit_test();

    boost::unit_test::init_unit_test_func init_func = &init_unit_test;
#else
    extern ::boost::unit_test::test_suite* init_unit_test_suite( int argc, char* argv[] );

    boost::unit_test::init_unit_test_func init_func = &init_unit_test_suite;
#endif

    return ::boost::unit_test::unit_test_main( init_func, argc, argv );
}

#endif // !BOOST_TEST_DYN_LINK && !BOOST_TEST_NO_MAIN

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_UNIT_TEST_MAIN_IPP_012205GER
