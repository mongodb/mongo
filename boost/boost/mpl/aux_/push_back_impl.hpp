
#ifndef BOOST_MPL_AUX_PUSH_BACK_IMPL_HPP_INCLUDED
#define BOOST_MPL_AUX_PUSH_BACK_IMPL_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2000-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Source: /cvsroot/boost/boost/boost/mpl/aux_/push_back_impl.hpp,v $
// $Date: 2004/09/02 15:40:44 $
// $Revision: 1.4 $

#include <boost/mpl/push_back_fwd.hpp>
#include <boost/mpl/aux_/has_type.hpp>
#include <boost/mpl/aux_/traits_lambda_spec.hpp>
#include <boost/mpl/aux_/config/forwarding.hpp>
#include <boost/mpl/aux_/config/static_constant.hpp>

namespace boost { namespace mpl {

// agurt 05/feb/04: no default implementation; the stub definition is needed 
// to enable the default 'has_push_back' implementation below
template< typename Tag >
struct push_back_impl
{
    template< typename Sequence, typename T > struct apply {};
};

template< typename Tag >
struct has_push_back_impl
{
    template< typename Seq > struct apply
#if !defined(BOOST_MPL_CFG_NO_NESTED_FORWARDING)
        : aux::has_type< push_back<Seq,int> >
    {
#else
    {
        typedef aux::has_type< push_back<Seq,int> > type;
        BOOST_STATIC_CONSTANT(bool, value = 
              (aux::has_type< push_back<Seq,int> >::value)
            );
#endif
    };
};

BOOST_MPL_ALGORITM_TRAITS_LAMBDA_SPEC(2, push_back_impl)
BOOST_MPL_ALGORITM_TRAITS_LAMBDA_SPEC(1, has_push_back_impl)

}}

#endif // BOOST_MPL_AUX_PUSH_BACK_IMPL_HPP_INCLUDED
