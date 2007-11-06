//  (C) Copyright Gennadiy Rozental 2003-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test_log_formatter.hpp,v $
//
//  Version     : $Revision: 1.14 $
//
//  Description : 
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_LOG_FORMATTER_HPP_071894GER
#define BOOST_TEST_UNIT_TEST_LOG_FORMATTER_HPP_071894GER

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/detail/log_level.hpp>
#include <boost/test/detail/fwd_decl.hpp>

// STL
#include <iosfwd>
#include <string> // for std::string

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                log_entry_data                ************** //
// ************************************************************************** //

struct BOOST_TEST_DECL log_entry_data {
    log_entry_data()
    {
        m_file_name.reserve( 200 );
    }

    std::string     m_file_name;
    std::size_t     m_line_num;
    log_level       m_level;

    void clear()
    {
        m_file_name.erase();
        m_line_num      = 0;
        m_level     = log_nothing;
    }
};

// ************************************************************************** //
// **************                checkpoint_data               ************** //
// ************************************************************************** //

struct BOOST_TEST_DECL log_checkpoint_data
{
    const_string    m_file_name;
    std::size_t     m_line_num;
    std::string     m_message;

    void clear()
    {
        m_file_name.clear();
        m_line_num    = 0;
        m_message = std::string();
    }
};

// ************************************************************************** //
// **************            unit_test_log_formatter           ************** //
// ************************************************************************** //

class BOOST_TEST_DECL unit_test_log_formatter {
public:
    enum log_entry_types { BOOST_UTL_ET_INFO, 
                           BOOST_UTL_ET_MESSAGE,
                           BOOST_UTL_ET_WARNING,
                           BOOST_UTL_ET_ERROR,
                           BOOST_UTL_ET_FATAL_ERROR };

    // Destructor
    virtual             ~unit_test_log_formatter() {}

    // Formatter interface
    virtual void        log_start( std::ostream&, counter_t test_cases_amount ) = 0;
    virtual void        log_finish( std::ostream& ) = 0;
    virtual void        log_build_info( std::ostream& ) = 0;

    virtual void        test_unit_start( std::ostream&, test_unit const& tu ) = 0;
    virtual void        test_unit_finish( std::ostream&, test_unit const& tu, unsigned long elapsed ) = 0;
    virtual void        test_unit_skipped( std::ostream&, test_unit const& ) = 0;

    virtual void        log_exception( std::ostream&, log_checkpoint_data const&, const_string explanation ) = 0;

    virtual void        log_entry_start( std::ostream&, log_entry_data const&, log_entry_types let ) = 0;
    virtual void        log_entry_value( std::ostream&, const_string value ) = 0;
    virtual void        log_entry_finish( std::ostream& ) = 0;
};

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: unit_test_log_formatter.hpp,v $
//  Revision 1.14  2005/12/14 05:52:16  rogeeff
//  log API simplified
//
//  Revision 1.13  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.12  2005/02/01 08:59:28  rogeeff
//  supplied_log_formatters split
//  change formatters interface to simplify result interface
//
//  Revision 1.11  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.10  2005/01/30 03:23:58  rogeeff
//  counter type renamed
//  log interface slightly shortened
//
//  Revision 1.9  2005/01/21 07:30:24  rogeeff
//  to log testing time log formatter interfaces changed
//
//  Revision 1.8  2005/01/18 08:26:12  rogeeff
//  unit_test_log rework:
//     eliminated need for ::instance()
//     eliminated need for << end and ...END macro
//     straitend interface between log and formatters
//     change compiler like formatter name
//     minimized unit_test_log interface and reworked to use explicit calls
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_LOG_FORMATTER_HPP_071894GER

