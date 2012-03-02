//  (C) Copyright Gennadiy Rozental 2001-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 63640 $
//
//  Description : simple implementation for Unit Test Framework parameter
//  handling routines. May be rewritten in future to use some kind of
//  command-line arguments parsing facility and environment variable handling
//  facility
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_PARAMETERS_IPP_012205GER
#define BOOST_TEST_UNIT_TEST_PARAMETERS_IPP_012205GER

// Boost.Test
#include <boost/test/detail/unit_test_parameters.hpp>
#include <boost/test/utils/basic_cstring/basic_cstring.hpp>
#include <boost/test/utils/basic_cstring/compare.hpp>
#include <boost/test/utils/basic_cstring/io.hpp>
#include <boost/test/utils/fixed_mapping.hpp>
#include <boost/test/debug.hpp>
#include <boost/test/framework.hpp>

// Boost.Runtime.Param
#include <boost/test/utils/runtime/cla/dual_name_parameter.hpp>
#include <boost/test/utils/runtime/cla/parser.hpp>

namespace rt  = boost::runtime;
namespace cla = rt::cla;


#ifndef UNDER_CE
#include <boost/test/utils/runtime/env/variable.hpp>

namespace env = rt::env;
#endif


// Boost
#include <boost/config.hpp>
#include <boost/test/detail/suppress_warnings.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/test/detail/enable_warnings.hpp>

// STL
#include <map>
#include <cstdlib>
#include <iostream>
#include <fstream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

# ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::getenv; using ::strncmp; using ::strcmp; }
# endif

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************    input operations for unit_test's enums    ************** //
// ************************************************************************** //

std::istream&
operator>>( std::istream& in, unit_test::log_level& ll )
{
    static fixed_mapping<const_string,unit_test::log_level,case_ins_less<char const> > log_level_name(
        "all"           , log_successful_tests,
        "success"       , log_successful_tests,
        "test_suite"    , log_test_units,
        "unit_scope"    , log_test_units,
        "message"       , log_messages,
        "warning"       , log_warnings,
        "error"         , log_all_errors,
        "cpp_exception" , log_cpp_exception_errors,
        "system_error"  , log_system_errors,
        "fatal_error"   , log_fatal_errors,
        "nothing"       , log_nothing,

        invalid_log_level
        );

    std::string val;
    in >> val;

    ll = log_level_name[val];
    BOOST_TEST_SETUP_ASSERT( ll != unit_test::invalid_log_level, "invalid log level " + val );

    return in;
}

//____________________________________________________________________________//

std::istream&
operator>>( std::istream& in, unit_test::report_level& rl )
{
    fixed_mapping<const_string,unit_test::report_level,case_ins_less<char const> > report_level_name (
        "confirm",  CONFIRMATION_REPORT,
        "short",    SHORT_REPORT,
        "detailed", DETAILED_REPORT,
        "no",       NO_REPORT,

        INV_REPORT_LEVEL
        );

    std::string val;
    in >> val;

    rl = report_level_name[val];
    BOOST_TEST_SETUP_ASSERT( rl != INV_REPORT_LEVEL, "invalid report level " + val );

    return in;
}

//____________________________________________________________________________//

std::istream&
operator>>( std::istream& in, unit_test::output_format& of )
{
    fixed_mapping<const_string,unit_test::output_format,case_ins_less<char const> > output_format_name (
        "HRF", unit_test::CLF,
        "CLF", unit_test::CLF,
        "XML", unit_test::XML,

        unit_test::INV_OF
        );

    std::string val;
    in >> val;

    of = output_format_name[val];
    BOOST_TEST_SETUP_ASSERT( of != unit_test::INV_OF, "invalid output format " + val );

    return in;
}

//____________________________________________________________________________//

// ************************************************************************** //
// **************                 runtime_config               ************** //
// ************************************************************************** //

namespace runtime_config {

namespace {

// framework parameters and corresponding command-line arguments
std::string AUTO_START_DBG    = "auto_start_dbg";
std::string BREAK_EXEC_PATH   = "break_exec_path";
std::string BUILD_INFO        = "build_info";
std::string CATCH_SYS_ERRORS  = "catch_system_errors";
std::string DETECT_FP_EXCEPT  = "detect_fp_exceptions";
std::string DETECT_MEM_LEAKS  = "detect_memory_leaks";
std::string LOG_FORMAT        = "log_format";
std::string LOG_LEVEL         = "log_level";
std::string LOG_SINK          = "log_sink";
std::string OUTPUT_FORMAT     = "output_format";
std::string RANDOM_SEED       = "random";
std::string REPORT_FORMAT     = "report_format";
std::string REPORT_LEVEL      = "report_level";
std::string REPORT_SINK       = "report_sink";
std::string RESULT_CODE       = "result_code";
std::string TESTS_TO_RUN      = "run_test";
std::string SAVE_TEST_PATTERN = "save_pattern";
std::string SHOW_PROGRESS     = "show_progress";
std::string USE_ALT_STACK     = "use_alt_stack";

fixed_mapping<const_string,const_string> parameter_2_env_var(
    AUTO_START_DBG    , "BOOST_TEST_AUTO_START_DBG",
    BREAK_EXEC_PATH   , "BOOST_TEST_BREAK_EXEC_PATH",
    BUILD_INFO        , "BOOST_TEST_BUILD_INFO",
    CATCH_SYS_ERRORS  , "BOOST_TEST_CATCH_SYSTEM_ERRORS",
    DETECT_FP_EXCEPT  , "BOOST_TEST_DETECT_FP_EXCEPTIONS",
    DETECT_MEM_LEAKS  , "BOOST_TEST_DETECT_MEMORY_LEAK",
    LOG_FORMAT        , "BOOST_TEST_LOG_FORMAT",
    LOG_LEVEL         , "BOOST_TEST_LOG_LEVEL",
    LOG_SINK          , "BOOST_TEST_LOG_SINK",
    OUTPUT_FORMAT     , "BOOST_TEST_OUTPUT_FORMAT",
    RANDOM_SEED       , "BOOST_TEST_RANDOM",
    REPORT_FORMAT     , "BOOST_TEST_REPORT_FORMAT",
    REPORT_LEVEL      , "BOOST_TEST_REPORT_LEVEL",
    REPORT_SINK       , "BOOST_TEST_REPORT_SINK",
    RESULT_CODE       , "BOOST_TEST_RESULT_CODE",
    TESTS_TO_RUN      , "BOOST_TESTS_TO_RUN",
    SAVE_TEST_PATTERN , "BOOST_TEST_SAVE_PATTERN",
    SHOW_PROGRESS     , "BOOST_TEST_SHOW_PROGRESS",
    USE_ALT_STACK     , "BOOST_TEST_USE_ALT_STACK",

    ""
);

//____________________________________________________________________________//

// storage for the CLAs
cla::parser     s_cla_parser;
std::string     s_empty;

output_format   s_report_format;
output_format   s_log_format;

//____________________________________________________________________________//

template<typename T>
T
retrieve_parameter( const_string parameter_name, cla::parser const& s_cla_parser, T const& default_value = T(), T const& optional_value = T() )
{
    rt::const_argument_ptr arg = s_cla_parser[parameter_name];
    if( arg ) {
        if( rtti::type_id<T>() == rtti::type_id<bool>() ||
            !static_cast<cla::parameter const&>( arg->p_formal_parameter.get() ).p_optional_value )
            return s_cla_parser.get<T>( parameter_name );

        optional<T> val = s_cla_parser.get<optional<T> >( parameter_name );
        if( val )
            return *val;
        else
            return optional_value;
    }

    boost::optional<T> v;

#ifndef UNDER_CE
    env::get( parameter_2_env_var[parameter_name], v );
#endif

    if( v )
        return *v;
    else
        return default_value;
}

//____________________________________________________________________________//

} // local namespace 

void
init( int& argc, char** argv )
{
    using namespace cla;

    try {
        s_cla_parser - cla::ignore_mismatch
          << cla::dual_name_parameter<bool>( AUTO_START_DBG + "|d" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Automatically starts debugger if system level error (signal) occurs")
          << cla::named_parameter<std::string>( BREAK_EXEC_PATH )
            - (cla::prefix = "--",cla::separator = "=",cla::guess_name,cla::optional,
               cla::description = "For the exception safety testing allows to break at specific execution path")
          << cla::dual_name_parameter<bool>( BUILD_INFO + "|i" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Shows library build information" )
          << cla::dual_name_parameter<bool>( CATCH_SYS_ERRORS + "|s" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Allows to switch between catching and ignoring system errors (signals)")
          << cla::named_parameter<bool>( DETECT_FP_EXCEPT )
            - (cla::prefix = "--",cla::separator = "=",cla::guess_name,cla::optional,
               cla::description = "Allows to switch between catching and ignoring floating point exceptions")
          << cla::named_parameter<long>( DETECT_MEM_LEAKS )
            - (cla::prefix = "--",cla::separator = "=",cla::guess_name,cla::optional,
               cla::description = "Allows to switch between catching and ignoring memory leaks")
          << cla::dual_name_parameter<unit_test::output_format>( LOG_FORMAT + "|f" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Specifies log format")
          << cla::dual_name_parameter<unit_test::log_level>( LOG_LEVEL + "|l" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Specifies log level")
          << cla::dual_name_parameter<std::string>( LOG_SINK + "|k" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Specifies log sink:stdout(default),stderr or file name")
          << cla::dual_name_parameter<unit_test::output_format>( OUTPUT_FORMAT + "|o" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Specifies output format (both log and report)")
          << cla::dual_name_parameter<int>( RANDOM_SEED + "|a" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,cla::optional_value,
               cla::description = "Allows to switch between sequential and random order of test units execution.\n"
                                  "Optionally allows to specify concrete seed for random number generator")
          << cla::dual_name_parameter<unit_test::output_format>( REPORT_FORMAT + "|m" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Specifies report format")
          << cla::dual_name_parameter<unit_test::report_level>(REPORT_LEVEL + "|r")
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Specifies report level")
          << cla::dual_name_parameter<std::string>( REPORT_SINK + "|e" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Specifies report sink:stderr(default),stdout or file name")
          << cla::dual_name_parameter<bool>( RESULT_CODE + "|c" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Allows to disable test modules's result code generation")
          << cla::dual_name_parameter<std::string>( TESTS_TO_RUN + "|t" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Allows to filter which test units to run")
          << cla::named_parameter<bool>( SAVE_TEST_PATTERN )
            - (cla::prefix = "--",cla::separator = "=",cla::guess_name,cla::optional,
               cla::description = "Allows to switch between saving and matching against test pattern file")
          << cla::dual_name_parameter<bool>( SHOW_PROGRESS + "|p" )
            - (cla::prefix = "--|-",cla::separator = "=| ",cla::guess_name,cla::optional,
               cla::description = "Turns on progress display")
          << cla::named_parameter<bool>( USE_ALT_STACK )
            - (cla::prefix = "--",cla::separator = "=",cla::guess_name,cla::optional,
               cla::description = "Turns on/off usage of an alternative stack for signal handling")

          << cla::dual_name_parameter<bool>( "help|?" )
            - (cla::prefix = "--|-",cla::separator = "=",cla::guess_name,cla::optional,
               cla::description = "this help message")
            ;

        s_cla_parser.parse( argc, argv );

        if( s_cla_parser["help"] ) {
            s_cla_parser.help( std::cout );
            throw framework::nothing_to_test();
        }

        s_report_format     = retrieve_parameter( REPORT_FORMAT, s_cla_parser, unit_test::CLF );
        s_log_format        = retrieve_parameter( LOG_FORMAT, s_cla_parser, unit_test::CLF );

        unit_test::output_format of = retrieve_parameter( OUTPUT_FORMAT, s_cla_parser, unit_test::INV_OF );

        if( of != unit_test::INV_OF )
            s_report_format = s_log_format = of;
    }
    catch( rt::logic_error const& ex ) {
        std::ostringstream err;
        
        err << "Fail to process runtime parameters: " << ex.msg() << std::endl;
        s_cla_parser.usage( err );

        throw framework::setup_error( err.str() );
    }
}

//____________________________________________________________________________//

unit_test::log_level
log_level()
{
    return retrieve_parameter( LOG_LEVEL, s_cla_parser, unit_test::log_all_errors );
}

//____________________________________________________________________________//

bool
no_result_code()
{
    return !retrieve_parameter( RESULT_CODE, s_cla_parser, true );
}

//____________________________________________________________________________//

unit_test::report_level
report_level()
{
    return retrieve_parameter( REPORT_LEVEL, s_cla_parser, unit_test::CONFIRMATION_REPORT );
}

//____________________________________________________________________________//

const_string
test_to_run()
{
    static std::string s_test_to_run = retrieve_parameter( TESTS_TO_RUN, s_cla_parser, s_empty );

    return s_test_to_run;
}

//____________________________________________________________________________//

const_string
break_exec_path()
{
    static std::string s_break_exec_path = retrieve_parameter( BREAK_EXEC_PATH, s_cla_parser, s_empty );

    return s_break_exec_path;
}

//____________________________________________________________________________//

bool
save_pattern()
{
    return retrieve_parameter( SAVE_TEST_PATTERN, s_cla_parser, false );
}

//____________________________________________________________________________//

bool
show_progress()
{
    return retrieve_parameter( SHOW_PROGRESS, s_cla_parser, false );
}

//____________________________________________________________________________//

bool
show_build_info()
{
    return retrieve_parameter( BUILD_INFO, s_cla_parser, false );
}

//____________________________________________________________________________//

bool
catch_sys_errors()
{
    return retrieve_parameter( CATCH_SYS_ERRORS, s_cla_parser, 
#ifdef BOOST_TEST_DEFAULTS_TO_CORE_DUMP
        false
#else
        true 
#endif
        );
}

//____________________________________________________________________________//

bool
auto_start_dbg()
{
    // !! set debugger as an option
    return retrieve_parameter( AUTO_START_DBG, s_cla_parser, false );
;
}

//____________________________________________________________________________//

bool
use_alt_stack()
{
    return retrieve_parameter( USE_ALT_STACK, s_cla_parser, true );
}

//____________________________________________________________________________//

bool
detect_fp_exceptions()
{
    return retrieve_parameter( DETECT_FP_EXCEPT, s_cla_parser, false );
}

//____________________________________________________________________________//

output_format
report_format()
{
    return s_report_format;
}

//____________________________________________________________________________//

output_format
log_format()
{
    return s_log_format;
}

//____________________________________________________________________________//

std::ostream*
report_sink()
{
    std::string sink_name = retrieve_parameter( REPORT_SINK, s_cla_parser, s_empty );

    if( sink_name.empty() || sink_name == "stderr" )
        return &std::cerr;    
    
    if( sink_name == "stdout" )
        return &std::cout;

    static std::ofstream log_file( sink_name.c_str() );
    return &log_file;
}

//____________________________________________________________________________//

std::ostream*
log_sink()
{
    std::string sink_name = retrieve_parameter( LOG_SINK, s_cla_parser, s_empty );

    if( sink_name.empty() || sink_name == "stdout" )
        return &std::cout;    

    if( sink_name == "stderr" )
        return &std::cerr;    

    static std::ofstream report_file( sink_name.c_str() );
    return &report_file;
}

//____________________________________________________________________________//

long
detect_memory_leaks()
{
    return retrieve_parameter( DETECT_MEM_LEAKS, s_cla_parser, static_cast<long>(1) );
}

//____________________________________________________________________________//

int
random_seed()
{
    return retrieve_parameter( RANDOM_SEED, s_cla_parser, 0, 1 );
}

//____________________________________________________________________________//

} // namespace runtime_config

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_UNIT_TEST_PARAMETERS_IPP_012205GER
