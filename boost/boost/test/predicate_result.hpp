//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: predicate_result.hpp,v $
//
//  Version     : $Revision: 1.7 $
//
//  Description : enhanced result for test predicate that include message explaining failure
// ***************************************************************************

#ifndef BOOST_TEST_PREDICATE_RESULT_HPP_012705GER
#define BOOST_TEST_PREDICATE_RESULT_HPP_012705GER

// Boost.Test
#include <boost/test/utils/class_properties.hpp>
#include <boost/test/utils/wrap_stringstream.hpp>
#include <boost/test/utils/basic_cstring/basic_cstring.hpp>

// Boost
#include <boost/shared_ptr.hpp>
#include <boost/detail/workaround.hpp>

// STL
#include <cstddef>          // for std::size_t

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace test_tools {

// ************************************************************************** //
// **************                predicate_result              ************** //
// ************************************************************************** //

class BOOST_TEST_DECL predicate_result {
    typedef unit_test::const_string      const_string;
public:
    // Constructor
    predicate_result( bool pv_ ) 
    : p_predicate_value( pv_ )
    {}

    template<typename BoolConvertable>
    predicate_result( BoolConvertable const& pv_ ) : p_predicate_value( !!pv_ ) {}

    bool                operator!() const           { return !p_predicate_value; }
    void                operator=( bool pv_ )       { p_predicate_value.value = pv_; }

    // Public properties
    BOOST_READONLY_PROPERTY( bool, (predicate_result) ) p_predicate_value;

    // Access methods
    bool                has_empty_message() const   { return !m_message; }
    wrap_stringstream&  message()
    {
        if( !m_message )
            m_message.reset( new wrap_stringstream );

        return *m_message;
    }
    const_string        message() const                   { return !m_message ? const_string() : const_string( m_message->str() ); }

private:
    // Data members
    shared_ptr<wrap_stringstream> m_message;
};

} // namespace test_tools

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: predicate_result.hpp,v $
//  Revision 1.7  2005/12/14 05:16:49  rogeeff
//  dll support introduced
//
//  Revision 1.6  2005/03/23 21:02:17  rogeeff
//  Sunpro CC 5.3 fixes
//
//  Revision 1.5  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.4  2005/02/03 20:39:12  rogeeff
//  m_message zero init for sunpro
//
//  Revision 1.3  2005/02/01 06:40:06  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.2  2005/01/31 20:07:19  rogeeff
//  Sunpro CC 5.3 workarounds
//
//  Revision 1.1  2005/01/30 03:24:51  rogeeff
//  extended_predicate_result renamed ot predicate_result and moved into separate file
//
// ***************************************************************************

#endif // BOOST_TEST_PREDICATE_RESULT_HPP_012705GER
