//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: xml_log_formatter.ipp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : implements XML Log formatter
// ***************************************************************************

#ifndef BOOST_TEST_XML_LOG_FORMATTER_IPP_020105GER
#define BOOST_TEST_XML_LOG_FORMATTER_IPP_020105GER

// Boost.Test
#include <boost/test/output/xml_log_formatter.hpp>
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/framework.hpp>
#include <boost/test/utils/basic_cstring/io.hpp>

#include <boost/test/utils/xml_printer.hpp>

// Boost
#include <boost/version.hpp>

// STL
#include <iostream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace output {

static const_string tu_type_name( test_unit const& tu )
{
    return tu.p_type == tut_case ? "TestCase" : "TestSuite";
}

// ************************************************************************** //
// **************               xml_log_formatter              ************** //
// ************************************************************************** //

void
xml_log_formatter::log_start( std::ostream& ostr, counter_t )
{
    ostr  << "<TestLog>";
}

//____________________________________________________________________________//

void
xml_log_formatter::log_finish( std::ostream& ostr )
{
    ostr  << "</TestLog>";
}

//____________________________________________________________________________//

void
xml_log_formatter::log_build_info( std::ostream& ostr )
{
    ostr  << "<BuildInfo"
            << " platform"  << attr_value() << BOOST_PLATFORM
            << " compiler"  << attr_value() << BOOST_COMPILER
            << " stl"       << attr_value() << BOOST_STDLIB
            << " boost=\""  << BOOST_VERSION/100000     << "."
                            << BOOST_VERSION/100 % 1000 << "."
                            << BOOST_VERSION % 100      << '\"'
            << "/>";
}

//____________________________________________________________________________//

void
xml_log_formatter::test_unit_start( std::ostream& ostr, test_unit const& tu )
{
    ostr << "<" << tu_type_name( tu ) << " name" << attr_value() << tu.p_name.get() << ">";
}

//____________________________________________________________________________//

void
xml_log_formatter::test_unit_finish( std::ostream& ostr, test_unit const& tu, unsigned long elapsed )
{
    if( tu.p_type == tut_case )
        ostr << "<TestingTime>" << elapsed << "</TestingTime>";
        
    ostr << "</" << tu_type_name( tu ) << ">";
}

//____________________________________________________________________________//

void
xml_log_formatter::test_unit_skipped( std::ostream& ostr, test_unit const& tu )
{
    ostr << "<" << tu_type_name( tu )
         << " name"    << attr_value() << tu.p_name.get()
         << " skipped" << attr_value() << "yes"
         << "/>";
}
    
//____________________________________________________________________________//

void
xml_log_formatter::log_exception( std::ostream& ostr, log_checkpoint_data const& checkpoint_data, const_string explanation )
{
    ostr << "<Exception name" << attr_value() << framework::current_test_case().p_name.get() << ">"
           << pcdata() << explanation;

    if( !checkpoint_data.m_file_name.is_empty() ) {
        ostr << "<LastCheckpoint file" << attr_value() << checkpoint_data.m_file_name
             << " line"                << attr_value() << checkpoint_data.m_line_num
             << ">"
             << pcdata() << checkpoint_data.m_message
             << "</LastCheckpoint>";
    }

    ostr << "</Exception>";
}

//____________________________________________________________________________//

void
xml_log_formatter::log_entry_start( std::ostream& ostr, log_entry_data const& entry_data, log_entry_types let )
{
    static literal_string xml_tags[] = { "Info", "Message", "Warning", "Error", "FatalError" };

    m_curr_tag = xml_tags[let];
    ostr << '<' << m_curr_tag
         << " file" << attr_value() << entry_data.m_file_name
         << " line" << attr_value() << entry_data.m_line_num
         << ">";
}

//____________________________________________________________________________//

void
xml_log_formatter::log_entry_value( std::ostream& ostr, const_string value )
{
    ostr << pcdata() << value;
}

//____________________________________________________________________________//

void
xml_log_formatter::log_entry_finish( std::ostream& ostr )
{
    ostr << "</" << m_curr_tag << ">";

    m_curr_tag.clear();
}

//____________________________________________________________________________//

} // namespace output

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: xml_log_formatter.ipp,v $
//  Revision 1.5  2005/12/14 05:39:43  rogeeff
//  *** empty log message ***
//
//  Revision 1.4  2005/04/30 16:47:14  rogeeff
//  warning supressed
//
//  Revision 1.3  2005/04/29 06:29:35  rogeeff
//  bug fix for incorect XML output
//
//  Revision 1.2  2005/02/20 08:27:07  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.1  2005/02/01 08:59:38  rogeeff
//  supplied_log_formatters split
//  change formatters interface to simplify result interface
//
// ***************************************************************************

#endif // BOOST_TEST_XML_LOG_FORMATTER_IPP_020105GER
