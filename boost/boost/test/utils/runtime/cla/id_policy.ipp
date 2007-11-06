//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: id_policy.ipp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : some generic identification policies implementation
// ***************************************************************************

#ifndef BOOST_RT_CLA_ID_POLICY_IPP_062904GER
#define BOOST_RT_CLA_ID_POLICY_IPP_062904GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

#include <boost/test/utils/runtime/cla/id_policy.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************              basic_naming_policy             ************** //
// ************************************************************************** //

BOOST_RT_PARAM_INLINE void
basic_naming_policy::usage_info( format_stream& fs ) const
{
    fs << m_prefix << m_name << m_separator;

    if( m_separator.empty() )
        fs << BOOST_RT_PARAM_LITERAL( ' ' );
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE bool
basic_naming_policy::match_prefix( argv_traverser& tr ) const
{
    if( !tr.match_front( m_prefix ) )
        return false;

    tr.trim( m_prefix.size() );
    return true;
}

//____________________________________________________________________________//
    
BOOST_RT_PARAM_INLINE bool
basic_naming_policy::match_name( argv_traverser& tr ) const
{
    if( !tr.match_front( m_name ) )
        return false;

    tr.trim( m_name.size() );
    return true;
}

//____________________________________________________________________________//
    
BOOST_RT_PARAM_INLINE bool
basic_naming_policy::match_separator( argv_traverser& tr ) const
{
    if( m_separator.empty() ) {
        if( !tr.token().is_empty() )
            return false;

        tr.trim( 1 );
    }
    else {
        if( !tr.match_front( m_separator ) )
            return false;

        tr.trim( m_separator.size() );
    }

    return true;
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE bool
basic_naming_policy::matching( parameter const&, argv_traverser& tr, bool ) const
{
    if( !match_prefix( tr ) )
        return false;
        
    if( !match_name( tr ) )
        return false;

    if( !match_separator( tr ) )
        return false;

    return true;
}

//____________________________________________________________________________//

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: id_policy.ipp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_ID_POLICY_IPP_062904GER
