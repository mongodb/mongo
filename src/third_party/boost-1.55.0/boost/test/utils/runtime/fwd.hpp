//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 49312 $
//
//  Description : global framework level forward declaration
// ***************************************************************************

#ifndef BOOST_RT_FWD_HPP_062604GER
#define BOOST_RT_FWD_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

// Boost
#include <boost/shared_ptr.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

class parameter;

class argument;
typedef shared_ptr<argument> argument_ptr;
typedef shared_ptr<argument const> const_argument_ptr;

template<typename T> class value_interpreter;
template<typename T> class typed_argument;

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

#endif // BOOST_RT_FWD_HPP_062604GER
