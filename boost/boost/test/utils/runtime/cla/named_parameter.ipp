//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: named_parameter.ipp,v $
//
//  Version     : $Revision: 1.2 $
//
//  Description : implements model of named parameter
// ***************************************************************************

#ifndef BOOST_RT_CLA_NAMED_PARAMETER_IPP_062904GER
#define BOOST_RT_CLA_NAMED_PARAMETER_IPP_062904GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

#include <boost/test/utils/runtime/cla/named_parameter.hpp>
#include <boost/test/utils/runtime/cla/char_parameter.hpp>

// Boost.Test
#include <boost/test/utils/algorithm.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************              string_name_policy              ************** //
// ************************************************************************** //

BOOST_RT_PARAM_INLINE 
string_name_policy::string_name_policy()
: basic_naming_policy( rtti::type_id<string_name_policy>() )
, m_guess_name( false )
{
    assign_op( m_prefix, BOOST_RT_PARAM_CSTRING_LITERAL( "-" ), 0 );
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE bool
string_name_policy::responds_to( cstring name ) const
{
    std::pair<cstring::iterator,dstring::const_iterator> mm_pos;

    mm_pos = unit_test::mismatch( name.begin(), name.end(), m_name.begin(), m_name.end() );

    return mm_pos.first == name.end() && (m_guess_name || (mm_pos.second == m_name.end()) );
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE bool
string_name_policy::conflict_with( identification_policy const& id ) const
{
    if( id.p_type_id == p_type_id ) {
        string_name_policy const& snp = static_cast<string_name_policy const&>( id );

        if( m_name.empty() || snp.m_name.empty() )
            return false;

        std::pair<dstring::const_iterator,dstring::const_iterator> mm_pos =
            unit_test::mismatch( m_name.begin(), m_name.end(), snp.m_name.begin(), snp.m_name.end() );

        return mm_pos.first != m_name.begin()                              &&  // there is common substring
                (m_guess_name       || (mm_pos.first  == m_name.end()) )   &&  // that match me
                (snp.m_guess_name   || (mm_pos.second == snp.m_name.end()) );  // and snp
    }
    
    if( id.p_type_id == rtti::type_id<char_name_policy>() ) {
        char_name_policy const& cnp = static_cast<char_name_policy const&>( id );
    
        return m_guess_name && unit_test::first_char( cstring( m_name ) ) == unit_test::first_char( cnp.id_2_report() );
    }
    
    return false;    
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE bool
string_name_policy::match_name( argv_traverser& tr ) const
{
    if( !m_guess_name )
        return basic_naming_policy::match_name( tr );
    else {
        cstring in = tr.input();

        std::pair<cstring::iterator,dstring::const_iterator> mm_pos;
        
        mm_pos = unit_test::mismatch( in.begin(), in.end(), m_name.begin(), m_name.end() );

        if( mm_pos.first == in.begin() )
            return false;

        tr.trim( mm_pos.first - in.begin() );
    }

    return true;
}

//____________________________________________________________________________//

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: named_parameter.ipp,v $
//   Revision 1.2  2005/04/12 07:01:36  rogeeff
//   exclude polymorphic_downcast
//
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_NAMED_PARAMETER_IPP_062904GER
