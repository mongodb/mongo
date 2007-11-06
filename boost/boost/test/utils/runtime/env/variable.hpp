//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: variable.hpp,v $
//
//  Version     : $Revision: 1.2 $
//
//  Description : defines model of program environment variable
// ***************************************************************************

#ifndef BOOST_RT_ENV_VARIABLE_HPP_062604GER
#define BOOST_RT_ENV_VARIABLE_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>
#include <boost/test/utils/runtime/fwd.hpp>
#include <boost/test/utils/runtime/parameter.hpp>
#include <boost/test/utils/runtime/argument.hpp>

#include <boost/test/utils/runtime/env/fwd.hpp>

// Boost
#include <boost/optional.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace environment {

// ************************************************************************** //
// **************      runtime::environment::variable_data     ************** //
// ************************************************************************** //

namespace rt_env_detail {

struct variable_data : public runtime::parameter {
    cstring         m_var_name;
    dstring         m_global_id;
    argument_ptr    m_value;
};

} // namespace rt_env_detail

// ************************************************************************** //
// **************     runtime::environment::variable_base      ************** //
// ************************************************************************** //

class variable_base {
public:
    explicit    variable_base( rt_env_detail::variable_data& data ) : m_data( &data ) {}

    // arguments access
    template<typename T>
    T const&    value() const
    {
        return arg_value<T>( *m_data->m_value );
    }

    template<typename T>
    void        value( boost::optional<T>& res ) const
    {
        if( has_value() )
            res = arg_value<T>( *m_data->m_value );
        else
            res.reset();
    }

    bool        has_value() const   { return m_data->m_value; }
    cstring     name() const        { return m_data->m_var_name; }

protected:
    // Data members
    rt_env_detail::variable_data*  m_data;
} ;

// ************************************************************************** //
// **************        runtime::environment::variable        ************** //
// ************************************************************************** //

template<typename T = cstring>
class variable : public variable_base {
public:
    // Constructors
    explicit    variable( cstring var_name );

    template<typename Modifiers>
    explicit    variable( cstring var_name, Modifiers const& m );

    explicit    variable( rt_env_detail::variable_data& data ) 
    : variable_base( data )                                 {}

    // other variable assignment
    void        operator=( variable const& v )              { m_data = v.m_data; }

    // access methods
    T const&    value() const                               { return variable_base::value<T>(); }

    using       variable_base::value;

    // Value assignment
    template<typename V>
    void        operator=( V const& v )
    {
        if( !has_value() )
            m_data->m_value.reset( new typed_argument<T>( *m_data ) );

        arg_value<T>( *m_data->m_value ) = v;

        rt_env_detail::sys_write_var( m_data->m_var_name, format_stream().ref() << value() );
    }
}; // class variable

//____________________________________________________________________________//

template<typename CharT, typename Tr,typename T>
inline std::basic_ostream<CharT,Tr>&
operator<<( std::basic_ostream<CharT,Tr>& os, variable<T> const& v )
{
    os << v.name() << '=';

    if( v.has_value() )
        os << v.value();

    return os;
}

//____________________________________________________________________________//

template<typename T, typename V>
inline bool
operator==( variable<T> ev, V const& v )
{
    return ev.has_value() && ev.value() == v;
}

//____________________________________________________________________________//

template<typename T, typename V>
inline bool
operator==( V const& v, variable<T> ev )
{
    return ev.has_value() && ev.value() == v;
}

//____________________________________________________________________________//

template<typename T, typename V>
inline bool
operator!=( variable<T> ev, V const& v )
{
    return !ev.has_value() || ev.value() != v;
}

//____________________________________________________________________________//

template<typename T, typename V>
inline bool
operator!=( V const& v, variable<T> ev )
{
    return !ev.has_value() || ev.value() != v;
}

//____________________________________________________________________________//

} // namespace environment

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
// ************************************************************************** //
// Implementation

#include <boost/test/utils/runtime/env/environment.hpp>

// ************************************************************************** //
// **************        runtime::environment::variable        ************** //
// ************************************************************************** //

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace environment {

template<typename T>
variable<T>::variable( cstring var_name )
: variable_base( environment::var<T>( var_name ) )
{}

//____________________________________________________________________________//

template<typename T>
template<typename Modifiers>
variable<T>::variable( cstring var_name, Modifiers const& m )
: variable_base( environment::var<T>( var_name, m ) )
{}

//____________________________________________________________________________//

} // namespace environment

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: variable.hpp,v $
//   Revision 1.2  2005/05/05 05:55:45  rogeeff
//   portability fixes
//
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_ENV_VARIABLE_HPP_062604GER
