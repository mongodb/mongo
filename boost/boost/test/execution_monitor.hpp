//  (C) Copyright Gennadiy Rozental 2001-2005.
//  (C) Copyright Beman Dawes 2001.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: execution_monitor.hpp,v $
//
//  Version     : $Revision: 1.25 $
//
//  Description : defines abstract monitor interfaces and implements execution exception
//  The original Boost Test Library included an implementation detail function
//  named catch_exceptions() which caught otherwise uncaught C++ exceptions.
//  It was derived from an existing test framework by Beman Dawes.  The
//  intent was to expand later to catch other detectable but platform dependent
//  error events like Unix signals or Windows structured C exceptions.
//
//  Requests from early adopters of the Boost Test Library included
//  configurable levels of error message detail, elimination of templates,
//  separation of error reporting, and making the catch_exceptions() facilities
//  available as a public interface.  Support for unit testing also stretched
//  the function based design.  Implementation within the header became less
//  attractive due to the need to include many huge system dependent headers,
//  although still preferable in certain cases.
//
//  All those issues have been addressed by introducing the class-based
//  design presented here.
// ***************************************************************************

#ifndef BOOST_TEST_EXECUTION_MONITOR_HPP_071894GER
#define BOOST_TEST_EXECUTION_MONITOR_HPP_071894GER

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/detail/fwd_decl.hpp>
#include <boost/test/utils/callback.hpp>

// Boost
#include <boost/scoped_ptr.hpp>
#include <boost/type.hpp>
#include <boost/cstdlib.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace detail {

// ************************************************************************** //
// **************       detail::translate_exception_base       ************** //
// ************************************************************************** //

class BOOST_TEST_DECL translate_exception_base {
public:
    // Constructor
    explicit    translate_exception_base( boost::scoped_ptr<translate_exception_base>& next )
    {
        next.swap( m_next );
    }

    // Destructor
    virtual     ~translate_exception_base() {}

    virtual int operator()( unit_test::callback0<int> const& F ) = 0;

protected:
    // Data members
    boost::scoped_ptr<translate_exception_base> m_next;
};

} // namespace detail

// ************************************************************************** //
// **************              execution_exception             ************** //
// ************************************************************************** //
    
//  design rationale: fear of being out (or nearly out) of memory.
    
class BOOST_TEST_DECL execution_exception {
    typedef boost::unit_test::const_string const_string;
public:
    enum error_code {
        //  These values are sometimes used as program return codes.
        //  The particular values have been chosen to avoid conflicts with
        //  commonly used program return codes: values < 100 are often user
        //  assigned, values > 255 are sometimes used to report system errors.
        //  Gaps in values allow for orderly expansion.
        
        no_error               = 0,   // for completeness only; never returned
        user_error             = 200, // user reported non-fatal error
        cpp_exception_error    = 205, // see note (1) below
        system_error           = 210, // see note (2) below
        timeout_error          = 215, // only detectable on certain platforms
        user_fatal_error       = 220, // user reported fatal error
        system_fatal_error     = 225  // see note (2) below
        
        //  Note 1: Only uncaught C++ exceptions are treated as errors.
        //  If the application catches a C++ exception, it will never reach
        //  the execution_monitor.
        
        //  Note 2: These errors include Unix signals and Windows structured
        //  exceptions.  They are often initiated by hardware traps.
        //
        //  The implementation decides what is a fatal_system_exception and what is
        //  just a system_exception.  Fatal errors are so likely to have corrupted
        //  machine state (like a stack overflow or addressing exception) that it
        //  is unreasonable to continue execution.
    };
    
    // Constructor
    execution_exception( error_code ec_, const_string what_msg_ ) // max length 256 inc '\0'
    : m_error_code( ec_ ), m_what( what_msg_ ) {}

    // access methods
    error_code      code() const { return m_error_code; }
    const_string    what() const { return m_what; }

private:
    // Data members
    error_code      m_error_code;
    const_string    m_what;
}; // execution_exception

// ************************************************************************** //
// **************               execution_monitor              ************** //
// ************************************************************************** //

class BOOST_TEST_DECL execution_monitor {
public:
    int execute( unit_test::callback0<int> const& F, bool catch_system_errors = true, int timeout = 0 ); 
    //  The catch_system_errors parameter specifies whether the monitor should 
    //  try to catch system errors/exceptions that would cause program to crash 
    //  in regular case
    //  The timeout argument specifies the seconds that elapse before
    //  a timer_error occurs.  May be ignored on some platforms.
    //
    //  Returns:  Value returned by function().
    //
    //  Effects:  Calls executes supplied function F inside a try/catch block which also may
    //  include other unspecified platform dependent error detection code.
    //
    //  Throws: execution_exception on an uncaught C++ exception,
    //  a hardware or software signal, trap, or other exception.
    //
    //  Note: execute() doesn't consider it an error for F to return a non-zero value.
    
    // register custom (user supplied) exception translator
    template<typename Exception, typename ExceptionTranslator>
    void        register_exception_translator( ExceptionTranslator const& tr, boost::type<Exception>* = 0 );

private:
    // implementation helpers
    int         catch_signals( unit_test::callback0<int> const& F, bool catch_system_errors, int timeout );

    // Data members
    boost::scoped_ptr<detail::translate_exception_base> m_custom_translators;
}; // execution_monitor

namespace detail {

// ************************************************************************** //
// **************         detail::translate_exception          ************** //
// ************************************************************************** //

template<typename Exception, typename ExceptionTranslator>
class translate_exception : public translate_exception_base
{
    typedef boost::scoped_ptr<translate_exception_base> base_ptr;
public:
    explicit    translate_exception( ExceptionTranslator const& tr, base_ptr& next )
    : translate_exception_base( next ), m_translator( tr ) {}

    int operator()( unit_test::callback0<int> const& F )
    {
        try {
            return m_next ? (*m_next)( F ) : F();
        } catch( Exception const& e ) {
            m_translator( e );
            return boost::exit_exception_failure;
        }
    }

private:
    // Data members
    ExceptionTranslator m_translator;
};

} // namespace detail

template<typename Exception, typename ExceptionTranslator>
void
execution_monitor::register_exception_translator( ExceptionTranslator const& tr, boost::type<Exception>* )
{
    m_custom_translators.reset( 
        new detail::translate_exception<Exception,ExceptionTranslator>( tr,m_custom_translators ) );
}

// ************************************************************************** //
// **************              detect_memory_leaks             ************** //
// ************************************************************************** //

// turn on system memory leak detection
void BOOST_TEST_DECL detect_memory_leaks( bool on_off );
// break program execution on mem_alloc_order_num's allocation
void BOOST_TEST_DECL break_memory_alloc( long mem_alloc_order_num );

// ************************************************************************** //
// **************               execution_aborted              ************** //
// ************************************************************************** //

struct BOOST_TEST_DECL execution_aborted {};

}  // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: execution_monitor.hpp,v $
//  Revision 1.25  2006/01/30 07:29:49  rogeeff
//  split memory leaks detection API in two to get more functions with better defined roles
//
//  Revision 1.24  2005/12/14 05:05:58  rogeeff
//  dll support introduced
//
//  Revision 1.23  2005/04/05 06:11:37  rogeeff
//  memory leak allocation point detection\nextra help with _WIN32_WINNT
//
//  Revision 1.22  2005/02/20 08:27:05  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.21  2005/02/01 08:59:28  rogeeff
//  supplied_log_formatters split
//  change formatters interface to simplify result interface
//
//  Revision 1.20  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.19  2005/01/31 05:59:18  rogeeff
//  detect_memory_leak feature added
//
// ***************************************************************************

#endif
