//  (C) Copyright Gennadiy Rozental 2002-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: minimal.hpp,v $
//
//  Version     : $Revision: 1.19 $
//
//  Description : simple minimal testing definitions and implementation
// ***************************************************************************

#ifndef BOOST_TEST_MINIMAL_HPP_071894GER
#define BOOST_TEST_MINIMAL_HPP_071894GER

#define BOOST_CHECK(exp)       \
  ( (exp)                      \
      ? static_cast<void>(0)   \
      : boost::minimal_test::report_error(#exp,__FILE__,__LINE__, BOOST_CURRENT_FUNCTION) )

#define BOOST_REQUIRE(exp)     \
  ( (exp)                      \
      ? static_cast<void>(0)   \
      : boost::minimal_test::report_critical_error(#exp,__FILE__,__LINE__,BOOST_CURRENT_FUNCTION))

#define BOOST_ERROR( msg_ )    \
        boost::minimal_test::report_error( (msg_),__FILE__,__LINE__, BOOST_CURRENT_FUNCTION, true )
#define BOOST_FAIL( msg_ )     \
        boost::minimal_test::report_critical_error( (msg_),__FILE__,__LINE__, BOOST_CURRENT_FUNCTION, true )

//____________________________________________________________________________//

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/impl/execution_monitor.ipp>
#include <boost/test/utils/class_properties.hpp>
#include <boost/test/utils/basic_cstring/io.hpp>

// Boost
#include <boost/cstdlib.hpp>            // for exit codes#include <boost/cstdlib.hpp>            // for exit codes
#include <boost/current_function.hpp>   // for BOOST_CURRENT_FUNCTION

// STL
#include <iostream>                     // std::cerr, std::endl
#include <string>                       // std::string

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

int test_main( int argc, char* argv[] );  // prototype for users test_main()

namespace boost {
namespace minimal_test {

typedef boost::unit_test::const_string const_string;

inline unit_test::counter_t& errors_counter() { static unit_test::counter_t ec = 0; return ec; }

inline void
report_error( const char* msg, const char* file, int line, const_string func_name, bool is_msg = false )
{
    ++errors_counter();
    std::cerr << file << "(" << line << "): ";

    if( is_msg )
        std::cerr << msg;
    else
        std::cerr << "test " << msg << " failed";

    if( func_name != "(unknown)" )
        std::cerr << " in function: '" << func_name << "'";

    std::cerr << std::endl;
}

inline void
report_critical_error( const char* msg, const char* file, int line, const_string func_name, bool is_msg = false )
{
    report_error( msg, file, line, func_name, is_msg );

    throw boost::execution_aborted();
}

class caller {
public:
    // constructor
    caller( int argc, char** argv )
    : m_argc( argc ), m_argv( argv ) {}

    // execution monitor hook implementation
    int operator()() { return test_main( m_argc, m_argv ); }

private:
    // Data members
    int         m_argc;
    char**      m_argv;
}; // monitor

} // namespace minimal_test

} // namespace boost

//____________________________________________________________________________//

int BOOST_TEST_CALL_DECL main( int argc, char* argv[] )
{
    using namespace boost::minimal_test;

    try {
        ::boost::execution_monitor ex_mon;
        int run_result = ex_mon.execute( caller( argc, argv ) );

        BOOST_CHECK( run_result == 0 || run_result == boost::exit_success );
    }
    catch( boost::execution_exception const& exex ) {
        if( exex.code() != boost::execution_exception::no_error )
            BOOST_ERROR( (std::string( "exception \"" ).
                            append( exex.what().begin(), exex.what().end() ).
                            append( "\" caught" ) ).c_str() );
        std::cerr << "\n**** Testing aborted.";
    }

    if( boost::minimal_test::errors_counter() != 0 ) {
        std::cerr << "\n**** " << errors_counter()
                  << " error" << (errors_counter() > 1 ? "s" : "" ) << " detected\n";

        return boost::exit_test_failure;
    }

    std::cout << "\n**** no errors detected\n";
    
    return boost::exit_success;
}

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: minimal.hpp,v $
//  Revision 1.19  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.18  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.17  2005/01/31 07:50:05  rogeeff
//  cdecl portability fix
//
//  Revision 1.16  2005/01/31 06:01:27  rogeeff
//  BOOST_TEST_CALL_DECL correctness fixes
//
//  Revision 1.15  2005/01/22 19:22:12  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
// ***************************************************************************


#endif // BOOST_TEST_MINIMAL_HPP_071894GER
