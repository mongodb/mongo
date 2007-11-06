//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test_log.hpp,v $
//
//  Version     : $Revision: 1.32 $
//
//  Description : defines singleton class unit_test_log and all manipulators.
//  unit_test_log has output stream like interface. It's implementation is
//  completely hidden with pimple idiom
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_LOG_HPP_071894GER
#define BOOST_TEST_UNIT_TEST_LOG_HPP_071894GER

// Boost.Test
#include <boost/test/test_observer.hpp>

#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/detail/log_level.hpp>
#include <boost/test/detail/fwd_decl.hpp>

#include <boost/test/utils/wrap_stringstream.hpp>
#include <boost/test/utils/trivial_singleton.hpp>

// Boost
#include <boost/utility.hpp>

// STL
#include <iosfwd>   // for std::ostream&

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                log manipulators              ************** //
// ************************************************************************** //

namespace log {

struct BOOST_TEST_DECL begin {
    begin( const_string fn, std::size_t ln )
    : m_file_name( fn )
    , m_line_num( ln )
    {}

    const_string m_file_name;
    std::size_t m_line_num;
};

struct end {};

} // namespace log

// ************************************************************************** //
// **************             entry_value_collector            ************** //
// ************************************************************************** //

namespace ut_detail {

class BOOST_TEST_DECL entry_value_collector {
public:
    // Constructors
    entry_value_collector() : m_last( true ) {}
    entry_value_collector( entry_value_collector& rhs ) : m_last( true ) { rhs.m_last = false; }
    ~entry_value_collector();

    // collection interface
    entry_value_collector operator<<( const_string );

private:
    // Data members
    bool    m_last;
};

} // namespace ut_detail

// ************************************************************************** //
// **************                 unit_test_log                ************** //
// ************************************************************************** //

class BOOST_TEST_DECL unit_test_log_t : public test_observer, public singleton<unit_test_log_t> {
public:
    // test_observer interface implementation
    void                test_start( counter_t test_cases_amount );
    void                test_finish();
    void                test_aborted();

    void                test_unit_start( test_unit const& );
    void                test_unit_finish( test_unit const&, unsigned long elapsed );
    void                test_unit_skipped( test_unit const& );
    void                test_unit_aborted( test_unit const& );

    void                assertion_result( bool passed );
    void                exception_caught( execution_exception const& );

    virtual int         priority() { return 1; }

    // log configuration methods
    void                set_stream( std::ostream& );
    void                set_threshold_level( log_level );
    void                set_format( output_format );
    void                set_formatter( unit_test_log_formatter* );

    // test progress logging
    void                set_checkpoint( const_string file, std::size_t line_num, const_string msg = const_string() );

    // entry logging
    unit_test_log_t&    operator<<( log::begin const& );        // begin entry 
    unit_test_log_t&    operator<<( log::end const& );          // end entry
    unit_test_log_t&    operator<<( log_level );                // set entry level
    unit_test_log_t&    operator<<( const_string );             // log entry value

    ut_detail::entry_value_collector operator()( log_level );   // initiate entry collection

private:
    BOOST_TEST_SINGLETON_CONS( unit_test_log_t );
}; // unit_test_log_t

BOOST_TEST_SINGLETON_INST( unit_test_log )

// helper macros
#define BOOST_TEST_LOG_ENTRY( ll )                                                  \
    (::boost::unit_test::unit_test_log                                              \
        << ::boost::unit_test::log::begin( BOOST_TEST_L(__FILE__), __LINE__ ))(ll)  \
/**/

} // namespace unit_test

} // namespace boost

// ************************************************************************** //
// **************       Unit test log interface helpers        ************** //
// ************************************************************************** //

#define BOOST_TEST_MESSAGE( M )                                 \
    BOOST_TEST_LOG_ENTRY( ::boost::unit_test::log_messages )    \
    << (boost::wrap_stringstream().ref() << M).str()            \
/**/

//____________________________________________________________________________//

#define BOOST_TEST_PASSPOINT()                          \
    ::boost::unit_test::unit_test_log.set_checkpoint(   \
        BOOST_TEST_L(__FILE__),                         \
        (std::size_t)__LINE__ )                         \
/**/

//____________________________________________________________________________//

#define BOOST_TEST_CHECKPOINT( M )                      \
    ::boost::unit_test::unit_test_log.set_checkpoint(   \
        BOOST_TEST_L(__FILE__),                         \
        (std::size_t)__LINE__,                          \
        (boost::wrap_stringstream().ref() << M).str() ) \
/**/

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: unit_test_log.hpp,v $
//  Revision 1.32  2006/01/28 08:57:02  rogeeff
//  VC6.0 workaround removed
//
//  Revision 1.31  2005/12/14 05:23:21  rogeeff
//  dll support introduced
//  Minor interface simplifications
//  BOOST_TEST_MESSAGE and BOOST_TEST_CHECKPOINT moved into log realm
//  BOOST_TEST_PASSPOINT is introduced
//
//  Revision 1.30  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.29  2005/02/02 12:08:14  rogeeff
//  namespace log added for log manipulators
//
//  Revision 1.28  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.27  2005/01/30 03:26:29  rogeeff
//  return an ability for explicit end()
//
//  Revision 1.26  2005/01/21 07:30:24  rogeeff
//  to log testing time log formatter interfaces changed
//
//  Revision 1.25  2005/01/18 08:26:12  rogeeff
//  unit_test_log rework:
//     eliminated need for ::instance()
//     eliminated need for << end and ...END macro
//     straitend interface between log and formatters
//     change compiler like formatter name
//     minimized unit_test_log interface and reworked to use explicit calls
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_LOG_HPP_071894GER

