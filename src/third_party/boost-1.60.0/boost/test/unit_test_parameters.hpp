//  (C) Copyright Gennadiy Rozental 2001.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
/// @file
/// @brief Provides access to various Unit Test Framework runtime parameters
///
/// Primarily for use by the framework itself
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_PARAMETERS_HPP_071894GER
#define BOOST_TEST_UNIT_TEST_PARAMETERS_HPP_071894GER

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/utils/runtime/argument.hpp>

// STL
#include <iostream>
#include <fstream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {
namespace unit_test {
namespace runtime_config {

// ************************************************************************** //
// **************                 runtime_config               ************** //
// ************************************************************************** //

// UTF parameters 
BOOST_TEST_DECL extern std::string AUTO_START_DBG;
BOOST_TEST_DECL extern std::string BREAK_EXEC_PATH;
BOOST_TEST_DECL extern std::string BUILD_INFO;
BOOST_TEST_DECL extern std::string CATCH_SYS_ERRORS;
BOOST_TEST_DECL extern std::string COLOR_OUTPUT;
BOOST_TEST_DECL extern std::string DETECT_FP_EXCEPT;
BOOST_TEST_DECL extern std::string DETECT_MEM_LEAKS;
BOOST_TEST_DECL extern std::string LIST_CONTENT;
BOOST_TEST_DECL extern std::string LIST_LABELS;
BOOST_TEST_DECL extern std::string LOG_FORMAT;
BOOST_TEST_DECL extern std::string LOG_LEVEL;
BOOST_TEST_DECL extern std::string LOG_SINK;
BOOST_TEST_DECL extern std::string OUTPUT_FORMAT;
BOOST_TEST_DECL extern std::string RANDOM_SEED;
BOOST_TEST_DECL extern std::string REPORT_FORMAT;
BOOST_TEST_DECL extern std::string REPORT_LEVEL;
BOOST_TEST_DECL extern std::string REPORT_MEM_LEAKS;
BOOST_TEST_DECL extern std::string REPORT_SINK;
BOOST_TEST_DECL extern std::string RESULT_CODE;
BOOST_TEST_DECL extern std::string RUN_FILTERS;
BOOST_TEST_DECL extern std::string SAVE_TEST_PATTERN;
BOOST_TEST_DECL extern std::string SHOW_PROGRESS;
BOOST_TEST_DECL extern std::string USE_ALT_STACK;
BOOST_TEST_DECL extern std::string WAIT_FOR_DEBUGGER;

BOOST_TEST_DECL void init( int& argc, char** argv );

// ************************************************************************** //
// **************              runtime_param::get              ************** //
// ************************************************************************** //

/// Access to arguments
BOOST_TEST_DECL runtime::arguments_store const& argument_store();

template<typename T>
inline T const&
get( runtime::cstring parameter_name )
{
    return argument_store().get<T>( parameter_name );
}

/// For public access
BOOST_TEST_DECL bool save_pattern();

// ************************************************************************** //
// **************                  stream_holder               ************** //
// ************************************************************************** //

class stream_holder {
public:
    // Constructor
    explicit        stream_holder( std::ostream& default_stream )
    : m_stream( &default_stream )
    {
    }

    void            setup( runtime::cstring param_name )
    {
        if( !runtime_config::argument_store().has( param_name ) )
            return;

        std::string const& file_name = runtime_config::get<std::string>( param_name );

        if( file_name == "stderr" )
            m_stream = &std::cerr;
        else if( file_name == "stdout" )
            m_stream = &std::cout;
        else {
            m_file.open( file_name.c_str() );
            m_stream = &m_file;
        }
    }

    // Access methods
    std::ostream&   ref() const { return *m_stream; }  

private:
    // Data members
    std::ofstream   m_file;
    std::ostream*   m_stream;  
};

} // namespace runtime_config
} // namespace unit_test
} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_UNIT_TEST_PARAMETERS_HPP_071894GER
