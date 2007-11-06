//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: modifier.hpp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : parameter modifiers
// ***************************************************************************

#ifndef BOOST_RT_CLA_MODIFIER_HPP_062604GER
#define BOOST_RT_CLA_MODIFIER_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

// Boost.Test
#include <boost/test/utils/named_params.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************         environment variable modifiers       ************** //
// ************************************************************************** //

namespace {

nfp::typed_keyword<bool,struct optional_t>              optional_m;
nfp::named_parameter<bool const,optional_t>             optional( true );
nfp::typed_keyword<bool,struct required_t>              required_m;
nfp::named_parameter<bool const,required_t>             required( true );
nfp::typed_keyword<bool,struct multiplicable_t>         multiplicable_m;
nfp::named_parameter<bool const,multiplicable_t>        multiplicable( true );
nfp::typed_keyword<bool,struct guess_name_t>            guess_name_m;
nfp::named_parameter<bool const,guess_name_t>           guess_name( true );
nfp::typed_keyword<bool,struct ignore_mismatch_t>       ignore_mismatch_m;
nfp::named_parameter<bool const,ignore_mismatch_t>      ignore_mismatch( true );
nfp::typed_keyword<bool,struct optional_value_t>        optional_value_m;
nfp::named_parameter<bool const,optional_value_t>       optional_value( true );

nfp::typed_keyword<char_type,struct input_separator_t>  input_separator;
nfp::typed_keyword<cstring,struct prefix_t>             prefix;
nfp::typed_keyword<cstring,struct name_t>               name;
nfp::typed_keyword<cstring,struct separator_t>          separator;
nfp::typed_keyword<cstring,struct description_t>        description;
nfp::typed_keyword<cstring,struct refer_to_t>           default_refer_to;

nfp::keyword<struct default_value_t>                    default_value;
nfp::keyword<struct handler_t>                          handler;
nfp::keyword<struct interpreter_t>                      interpreter;
nfp::keyword<struct assign_to_t>                        assign_to;

} // local namespace

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: modifier.hpp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_MODIFIER_HPP_062604GER
