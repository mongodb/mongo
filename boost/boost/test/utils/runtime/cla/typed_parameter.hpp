//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: typed_parameter.hpp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : generic typed parameter model
// ***************************************************************************

#ifndef BOOST_RT_CLA_TYPED_PARAMETER_HPP_062604GER
#define BOOST_RT_CLA_TYPED_PARAMETER_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

#include <boost/test/utils/runtime/fwd.hpp>
#include <boost/test/utils/runtime/validation.hpp>

#include <boost/test/utils/runtime/cla/parameter.hpp>
#include <boost/test/utils/runtime/cla/argument_factory.hpp>

// Boost.Test
#include <boost/test/utils/rtti.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************         runtime::cla::typed_parameter        ************** //
// ************************************************************************** //

template<typename T>
class typed_parameter : public cla::parameter {
public:
    explicit typed_parameter( identification_policy& ID ) : cla::parameter( ID, m_arg_factory ) {}

    // parameter properties modification
    template<typename Modifier>
    void    accept_modifier( Modifier const& m )
    {
        cla::parameter::accept_modifier( m );

        m_arg_factory.accept_modifier( m );

        BOOST_RT_PARAM_VALIDATE_LOGIC( !p_optional || !m_arg_factory.m_value_generator,
            BOOST_RT_PARAM_LITERAL( "couldn't define a value generator for optional parameter " ) << id_2_report() );
    }

private:
    // Data members
    typed_argument_factory<T>   m_arg_factory;
};

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: typed_parameter.hpp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_TYPED_PARAMETER_HPP_062604GER
