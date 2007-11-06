//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: basic_parameter.hpp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : generic custom parameter generator
// ***************************************************************************

#ifndef BOOST_RT_CLA_BASIC_PARAMETER_HPP_062604GER
#define BOOST_RT_CLA_BASIC_PARAMETER_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

#include <boost/test/utils/runtime/cla/typed_parameter.hpp>

// Boost.Test
#include <boost/test/utils/rtti.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************         runtime::cla::basic_parameter        ************** //
// ************************************************************************** //

template<typename T, typename IdPolicy>
class basic_parameter : public typed_parameter<T> {
public:
    // Constructors
    explicit    basic_parameter( cstring n ) 
    : typed_parameter<T>( m_id_policy )
    {
        this->accept_modifier( name = n );
    }

    // parameter properties modification
    template<typename Modifier>
    void        accept_modifier( Modifier const& m )
    {
        typed_parameter<T>::accept_modifier( m );

        m_id_policy.accept_modifier( m );
    }

private:
    IdPolicy    m_id_policy;
};

//____________________________________________________________________________//

#define BOOST_RT_CLA_NAMED_PARAM_GENERATORS( param_type )                                       \
template<typename T>                                                                            \
inline shared_ptr<param_type ## _t<T> >                                                         \
param_type( cstring name = cstring() )                                                          \
{                                                                                               \
    return shared_ptr<param_type ## _t<T> >( new param_type ## _t<T>( name ) );                 \
}                                                                                               \
                                                                                                \
inline shared_ptr<param_type ## _t<cstring> >                                                   \
param_type( cstring name = cstring() )                                                          \
{                                                                                               \
    return shared_ptr<param_type ## _t<cstring> >( new param_type ## _t<cstring>( name ) );     \
}                                                                                               \
/**/

//____________________________________________________________________________//

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: basic_parameter.hpp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_BASIC_PARAMETER_HPP_062604GER
