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
//  Description : supplies offline implementation for the Test Tools
// ***************************************************************************

#ifndef BOOST_TEST_TEST_TOOLS_IPP_012205GER
#define BOOST_TEST_TEST_TOOLS_IPP_012205GER

// Boost.Test
#include <boost/test/test_tools.hpp>
#include <boost/test/unit_test_log.hpp>
#include <boost/test/output_test_stream.hpp>
#include <boost/test/framework.hpp>
#include <boost/test/execution_monitor.hpp> // execution_aborted
#include <boost/test/unit_test_suite_impl.hpp>

// Boost
#include <boost/config.hpp>

// STL
#include <fstream>
#include <string>
#include <cstring>
#include <cctype>
#include <cwchar>
#include <stdexcept>
#include <ios>

// !! should we use #include <cstdarg>
#include <stdarg.h>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

# ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::strcmp; using ::strlen; using ::isprint; }
#if !defined( BOOST_NO_CWCHAR )
namespace std { using ::wcscmp; }
#endif
# endif

namespace boost {

namespace test_tools {

// ************************************************************************** //
// **************                print_log_value               ************** //
// ************************************************************************** //

void
print_log_value<char>::operator()( std::ostream& ostr, char t )
{
    if( (std::isprint)( static_cast<unsigned char>(t) ) )
        ostr << '\'' << t << '\'';
    else
        ostr << std::hex
#if BOOST_TEST_USE_STD_LOCALE
        << std::showbase
#else
        << "0x"
#endif
        << static_cast<int>(t);
}

//____________________________________________________________________________//

void
print_log_value<unsigned char>::operator()( std::ostream& ostr, unsigned char t )
{
    ostr << std::hex
        // showbase is only available for new style streams:
#if BOOST_TEST_USE_STD_LOCALE
        << std::showbase
#else
        << "0x"
#endif
        << static_cast<int>(t);
}

//____________________________________________________________________________//

void
print_log_value<char const*>::operator()( std::ostream& ostr, char const* t )
{
    ostr << ( t ? t : "null string" );
}

//____________________________________________________________________________//

void
print_log_value<wchar_t const*>::operator()( std::ostream& ostr, wchar_t const* t )
{
    ostr << ( t ? t : L"null string" );
}

//____________________________________________________________________________//

namespace tt_detail {

// ************************************************************************** //
// **************            TOOL BOX Implementation           ************** //
// ************************************************************************** //

using ::boost::unit_test::lazy_ostream;

bool
check_impl( predicate_result const& pr, lazy_ostream const& check_descr,
            const_string file_name, std::size_t line_num,
            tool_level tl, check_type ct,
            std::size_t num_of_args, ... )
{
    using namespace unit_test;

    if( !framework::is_initialized() )
        throw std::runtime_error( "can't use testing tools before framework is initialized" );

    if( !!pr )
        tl = PASS;

    log_level    ll;
    char const*  prefix;
    char const*  suffix;
       
    switch( tl ) {
    case PASS:
        ll      = log_successful_tests;
        prefix  = "check ";
        suffix  = " passed";
        break;
    case WARN:
        ll      = log_warnings;
        prefix  = "condition ";
        suffix  = " is not satisfied";
        break;
    case CHECK:
        ll      = log_all_errors;
        prefix  = "check ";
        suffix  = " failed";
        break;
    case REQUIRE:
        ll      = log_fatal_errors;
        prefix  = "critical check ";
        suffix  = " failed";
        break;
    default:
        return true;
    }

    switch( ct ) {
    case CHECK_PRED:
        unit_test_log << unit_test::log::begin( file_name, line_num ) 
                      << ll << prefix << check_descr << suffix;
        
        if( !pr.has_empty_message() )
            unit_test_log << ". " << pr.message();
        
        unit_test_log << unit_test::log::end();
        break;

    case CHECK_MSG:
        unit_test_log << unit_test::log::begin( file_name, line_num ) << ll;
        
        if( tl == PASS )
            unit_test_log << prefix << "'" << check_descr << "'" << suffix;
        else
            unit_test_log << check_descr;
        
        if( !pr.has_empty_message() )
            unit_test_log << ". " << pr.message();

        unit_test_log << unit_test::log::end();
        break;

    case CHECK_EQUAL: 
    case CHECK_NE: 
    case CHECK_LT: 
    case CHECK_LE: 
    case CHECK_GT: 
    case CHECK_GE: {
        static char const* check_str [] = { " == ", " != ", " < " , " <= ", " > " , " >= " };
        static char const* rever_str [] = { " != ", " == ", " >= ", " > " , " <= ", " < "  };

        va_list args;

        va_start( args, num_of_args );
        char const*         arg1_descr  = va_arg( args, char const* );
        lazy_ostream const* arg1_val    = va_arg( args, lazy_ostream const* );
        char const*         arg2_descr  = va_arg( args, char const* );
        lazy_ostream const* arg2_val    = va_arg( args, lazy_ostream const* );

        unit_test_log << unit_test::log::begin( file_name, line_num ) 
                      << ll << prefix << arg1_descr << check_str[ct-CHECK_EQUAL] << arg2_descr << suffix;

        if( tl != PASS )
            unit_test_log << " [" << *arg1_val << rever_str[ct-CHECK_EQUAL] << *arg2_val << "]" ;

        va_end( args );
        
        if( !pr.has_empty_message() )
            unit_test_log << ". " << pr.message();

        unit_test_log << unit_test::log::end();
        break;
    }

    case CHECK_CLOSE:
    case CHECK_CLOSE_FRACTION: {
        va_list args;

        va_start( args, num_of_args );
        char const*         arg1_descr  = va_arg( args, char const* );
        lazy_ostream const* arg1_val    = va_arg( args, lazy_ostream const* );
        char const*         arg2_descr  = va_arg( args, char const* );
        lazy_ostream const* arg2_val    = va_arg( args, lazy_ostream const* );
        /* toler_descr = */               va_arg( args, char const* );
        lazy_ostream const* toler_val   = va_arg( args, lazy_ostream const* );

        unit_test_log << unit_test::log::begin( file_name, line_num ) << ll;

        unit_test_log << "difference{" << pr.message() << (ct == CHECK_CLOSE ? "%" : "")
                      << "} between " << arg1_descr << "{" << *arg1_val
                      << "} and "               << arg2_descr << "{" << *arg2_val
                      << ( tl == PASS ? "} doesn't exceed " : "} exceeds " )
                      << *toler_val;
        if( ct == CHECK_CLOSE )
            unit_test_log << "%";

        va_end( args );
        
        unit_test_log << unit_test::log::end();
        break;
    }
    case CHECK_SMALL: {
        va_list args;

        va_start( args, num_of_args );
        char const*         arg1_descr  = va_arg( args, char const* );
        lazy_ostream const* arg1_val    = va_arg( args, lazy_ostream const* );
        /* toler_descr = */               va_arg( args, char const* );
        lazy_ostream const* toler_val   = va_arg( args, lazy_ostream const* );

        unit_test_log << unit_test::log::begin( file_name, line_num ) << ll;

        unit_test_log << "absolute value of " << arg1_descr << "{" << *arg1_val << "}" 
                      << ( tl == PASS ? " doesn't exceed " : " exceeds " )
                      << *toler_val;

        va_end( args );
        
        if( !pr.has_empty_message() )
            unit_test_log << ". " << pr.message();

        unit_test_log << unit_test::log::end();
        break;
    }

    case CHECK_PRED_WITH_ARGS: {
        unit_test_log << unit_test::log::begin( file_name, line_num ) 
                      << ll << prefix << check_descr;

        // print predicate call description
        {
            va_list args;
            va_start( args, num_of_args );

            unit_test_log << "( ";
            for( std::size_t i = 0; i < num_of_args; ++i ) {
                unit_test_log << va_arg( args, char const* );
                va_arg( args, lazy_ostream const* ); // skip argument value;
                
                if( i != num_of_args-1 )
                    unit_test_log << ", ";
            }
            unit_test_log << " )" << suffix;
            va_end( args );
        }
                        
        if( tl != PASS ) {
            va_list args;
            va_start( args, num_of_args );

            unit_test_log << " for ( ";
            for( std::size_t i = 0; i < num_of_args; ++i ) {
                va_arg( args, char const* ); // skip argument description;            
                unit_test_log << *va_arg( args, lazy_ostream const* );
                
                if( i != num_of_args-1 )
                    unit_test_log << ", ";
            }
            unit_test_log << " )";
            va_end( args );
        }
       
        if( !pr.has_empty_message() )
            unit_test_log << ". " << pr.message();

        unit_test_log << unit_test::log::end();
        break;
    }

    case CHECK_EQUAL_COLL: {
        va_list args;

        va_start( args, num_of_args );
        char const* left_begin_descr    = va_arg( args, char const* );
        char const* left_end_descr      = va_arg( args, char const* );
        char const* right_begin_descr   = va_arg( args, char const* );
        char const* right_end_descr     = va_arg( args, char const* );

        unit_test_log << unit_test::log::begin( file_name, line_num ) 
                      << ll << prefix 
                      << "{ " << left_begin_descr  << ", " << left_end_descr  << " } == { " 
                              << right_begin_descr << ", " << right_end_descr << " }"
                      << suffix;

        va_end( args );
        
        if( !pr.has_empty_message() )
            unit_test_log << ". " << pr.message();

        unit_test_log << unit_test::log::end();
        break;
    }

    case CHECK_BITWISE_EQUAL: {
        va_list args;

        va_start( args, num_of_args );
        char const* left_descr    = va_arg( args, char const* );
        char const* right_descr   = va_arg( args, char const* );

        unit_test_log << unit_test::log::begin( file_name, line_num )
                      << ll << prefix << left_descr  << " =.= " << right_descr << suffix;

        va_end( args );
        
        if( !pr.has_empty_message() )
            unit_test_log << ". " << pr.message();

        unit_test_log << unit_test::log::end();
        break;
    }
    }

    switch( tl ) {
    case PASS:
        framework::assertion_result( true );
        return true;

    case WARN:
        return false;

    case CHECK:
        framework::assertion_result( false );
        return false;
        
    case REQUIRE:
        framework::assertion_result( false );

        framework::test_unit_aborted( framework::current_test_case() );

        throw execution_aborted();
    }

    return true;
}

//____________________________________________________________________________//

predicate_result
equal_impl( char const* left, char const* right )
{
    return (left && right) ? std::strcmp( left, right ) == 0 : (left == right);
}

//____________________________________________________________________________//

#if !defined( BOOST_NO_CWCHAR )

predicate_result
equal_impl( wchar_t const* left, wchar_t const* right )
{
    return (left && right) ? std::wcscmp( left, right ) == 0 : (left == right);
}

#endif // !defined( BOOST_NO_CWCHAR )

//____________________________________________________________________________//

bool
is_defined_impl( const_string symbol_name, const_string symbol_value )
{
    symbol_value.trim_left( 2 );
    return symbol_name != symbol_value;
}

//____________________________________________________________________________//

} // namespace tt_detail

// ************************************************************************** //
// **************               output_test_stream             ************** //
// ************************************************************************** //

struct output_test_stream::Impl
{
    std::fstream    m_pattern;
    bool            m_match_or_save;
    bool            m_text_or_binary;
    std::string     m_synced_string;

    char            get_char()
    {
        char res;
        do {
            m_pattern.get( res );
        } while( m_text_or_binary && res == '\r' && !m_pattern.fail() && !m_pattern.eof() );

        return res;
    }

    void            check_and_fill( predicate_result& res )
    {
        if( !res.p_predicate_value )
            res.message() << "Output content: \"" << m_synced_string << '\"';
    }
};

//____________________________________________________________________________//

output_test_stream::output_test_stream( const_string pattern_file_name, bool match_or_save, bool text_or_binary )
: m_pimpl( new Impl )
{
    if( !pattern_file_name.is_empty() ) {
        std::ios::openmode m = match_or_save ? std::ios::in : std::ios::out;
        if( !text_or_binary )
            m |= std::ios::binary;

        m_pimpl->m_pattern.open( pattern_file_name.begin(), m );

        BOOST_WARN_MESSAGE( m_pimpl->m_pattern.is_open(),
                             "Can't open pattern file " << pattern_file_name
                                << " for " << (match_or_save ? "reading" : "writing") );
    }

    m_pimpl->m_match_or_save    = match_or_save;
    m_pimpl->m_text_or_binary   = text_or_binary;
}

//____________________________________________________________________________//

output_test_stream::~output_test_stream()
{
    delete m_pimpl;
}

//____________________________________________________________________________//

predicate_result
output_test_stream::is_empty( bool flush_stream )
{
    sync();

    result_type res( m_pimpl->m_synced_string.empty() );

    m_pimpl->check_and_fill( res );

    if( flush_stream )
        flush();

    return res;
}

//____________________________________________________________________________//

predicate_result
output_test_stream::check_length( std::size_t length_, bool flush_stream )
{
    sync();

    result_type res( m_pimpl->m_synced_string.length() == length_ );

    m_pimpl->check_and_fill( res );

    if( flush_stream )
        flush();

    return res;
}

//____________________________________________________________________________//

predicate_result
output_test_stream::is_equal( const_string arg, bool flush_stream )
{
    sync();

    result_type res( const_string( m_pimpl->m_synced_string ) == arg );

    m_pimpl->check_and_fill( res );

    if( flush_stream )
        flush();

    return res;
}

//____________________________________________________________________________//

predicate_result
output_test_stream::match_pattern( bool flush_stream )
{
    sync();

    result_type result( true );

    if( !m_pimpl->m_pattern.is_open() ) {
        result = false;
        result.message() << "Pattern file can't be opened!";
    }
    else {
        if( m_pimpl->m_match_or_save ) {
            for ( std::string::size_type i = 0; i < m_pimpl->m_synced_string.length(); ++i ) {
                char c = m_pimpl->get_char();

                result = !m_pimpl->m_pattern.fail() &&
                         !m_pimpl->m_pattern.eof()  &&
                         (m_pimpl->m_synced_string[i] == c);

                if( !result ) {
                    std::string::size_type suffix_size  = (std::min)( m_pimpl->m_synced_string.length() - i,
                                                                    static_cast<std::string::size_type>(5) );

                    // try to log area around the mismatch
                    result.message() << "Mismatch at position " << i << '\n'
                        << "..." << m_pimpl->m_synced_string.substr( i, suffix_size ) << "..." << '\n'
                        << "..." << c;

                    std::string::size_type counter = suffix_size;
                    while( --counter ) {
                        char c = m_pimpl->get_char();

                        if( m_pimpl->m_pattern.fail() || m_pimpl->m_pattern.eof() )
                            break;

                        result.message() << c;
                    }

                    result.message() << "...";

                    // skip rest of the bytes. May help for further matching
                    m_pimpl->m_pattern.ignore( 
                        static_cast<std::streamsize>( m_pimpl->m_synced_string.length() - i - suffix_size) );
                    break;
                }
            }
        }
        else {
            m_pimpl->m_pattern.write( m_pimpl->m_synced_string.c_str(),
                                      static_cast<std::streamsize>( m_pimpl->m_synced_string.length() ) );
            m_pimpl->m_pattern.flush();
        }
    }

    if( flush_stream )
        flush();

    return result;
}

//____________________________________________________________________________//

void
output_test_stream::flush()
{
    m_pimpl->m_synced_string.erase();

#ifndef BOOST_NO_STRINGSTREAM
    str( std::string() );
#else
    seekp( 0, std::ios::beg );
#endif
}

//____________________________________________________________________________//

std::size_t
output_test_stream::length()
{
    sync();

    return m_pimpl->m_synced_string.length();
}

//____________________________________________________________________________//

void
output_test_stream::sync()
{
#ifdef BOOST_NO_STRINGSTREAM
    m_pimpl->m_synced_string.assign( str(), pcount() );
    freeze( false );
#else
    m_pimpl->m_synced_string = str();
#endif
}

//____________________________________________________________________________//

} // namespace test_tools

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_TEST_TOOLS_IPP_012205GER
