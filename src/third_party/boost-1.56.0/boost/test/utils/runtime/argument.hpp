//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
//
//  Description : model of actual argument (both typed and abstract interface)
// ***************************************************************************

#ifndef BOOST_RT_ARGUMENT_HPP_062604GER
#define BOOST_RT_ARGUMENT_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>
#include <boost/test/utils/runtime/fwd.hpp>
#include <boost/test/utils/runtime/validation.hpp>

// Boost.Test
#include <boost/test/utils/class_properties.hpp>
#include <boost/test/utils/rtti.hpp>

// STL
#include <cassert>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

// ************************************************************************** //
// **************              runtime::argument               ************** //
// ************************************************************************** //

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable:4244)
#endif

class argument {
public:
    // Constructor
    argument( parameter const& p, rtti::id_t value_type )
    : p_formal_parameter( p )
    , p_value_type( value_type )
    {}

    // Destructor
    virtual     ~argument()  {}

    // Public properties
    unit_test::readonly_property<parameter const&> p_formal_parameter;
    unit_test::readonly_property<rtti::id_t>       p_value_type;
};

// ************************************************************************** //
// **************             runtime::typed_argument          ************** //
// ************************************************************************** //

template<typename T>
class typed_argument : public argument {
public:
    // Constructor
    explicit typed_argument( parameter const& p )
    : argument( p, rtti::type_id<T>() )
    {}
    typed_argument( parameter const& p, T const& t )
    : argument( p, rtti::type_id<T>() )
    , p_value( t )
    {}

    unit_test::readwrite_property<T>    p_value;
};

// ************************************************************************** //
// **************               runtime::arg_value             ************** //
// ************************************************************************** //

template<typename T>
inline T const&
arg_value( argument const& arg_ )
{
    assert( arg_.p_value_type == rtti::type_id<T>() ); // detect logic error

    return static_cast<typed_argument<T> const&>( arg_ ).p_value.value;
}

//____________________________________________________________________________//

template<typename T>
inline T&
arg_value( argument& arg_ )
{
    assert( arg_.p_value_type == rtti::type_id<T>() ); // detect logic error

    return static_cast<typed_argument<T>&>( arg_ ).p_value.value;
}

#ifdef BOOST_MSVC
#  pragma warning(pop)
#endif

//____________________________________________________________________________//

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

#endif // BOOST_RT_ARGUMENT_HPP_062604GER
