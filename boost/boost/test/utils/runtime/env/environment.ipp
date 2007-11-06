//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: environment.ipp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : implements model of program environment 
// ***************************************************************************

#ifndef BOOST_RT_ENV_ENVIRONMENT_IPP_062904GER
#define BOOST_RT_ENV_ENVIRONMENT_IPP_062904GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>
#include <boost/test/utils/runtime/validation.hpp>

#include <boost/test/utils/runtime/env/variable.hpp>

// Boost.Test
#include <boost/test/utils/basic_cstring/compare.hpp>
#include <boost/test/utils/basic_cstring/io.hpp>

// STL
#include <map>
#include <list>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace environment {

// ************************************************************************** //
// **************             runtime::environment             ************** //
// ************************************************************************** //

namespace rt_env_detail {

typedef std::map<cstring,rt_env_detail::variable_data> registry;
typedef std::list<dstring> keys;

BOOST_RT_PARAM_INLINE registry& s_registry()    { static registry instance; return instance; }
BOOST_RT_PARAM_INLINE keys&     s_keys()        { static keys instance; return instance; }

BOOST_RT_PARAM_INLINE variable_data&
new_var_record( cstring var_name )
{
    // save the name in list of keys
    s_keys().push_back( dstring() );
    dstring& key = s_keys().back();
    assign_op( key, var_name, 0 );

    // create and return new record
    variable_data& new_var_data = s_registry()[key];
    
    new_var_data.m_var_name = key;
    
    return new_var_data;
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE variable_data*
find_var_record( cstring var_name )
{
    registry::iterator it = s_registry().find( var_name );

    return it == s_registry().end() ? 0 : &(it->second);
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE cstring
sys_read_var( cstring var_name )
{
    using namespace std;
    return BOOST_RT_PARAM_GETENV( var_name.begin() );
}

//____________________________________________________________________________//

BOOST_RT_PARAM_INLINE void
sys_write_var( cstring var_name, format_stream& var_value )
{
    BOOST_RT_PARAM_PUTENV( var_name, cstring( var_value.str() ) );
}

//____________________________________________________________________________//

} // namespace rt_env_detail

BOOST_RT_PARAM_INLINE variable_base
var( cstring var_name )
{
    rt_env_detail::variable_data* vd = rt_env_detail::find_var_record( var_name );

    BOOST_RT_PARAM_VALIDATE_LOGIC( !!vd,
                                   BOOST_RT_PARAM_LITERAL( "First access to the environment variable " ) 
                                        << var_name << BOOST_RT_PARAM_LITERAL( " should be typed" ) );

    return variable_base( *vd );
}

//____________________________________________________________________________//

} // namespace environment

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: environment.ipp,v $
//   Revision 1.5  2005/05/14 05:41:10  rogeeff
//   *** empty log message ***
//
//   Revision 1.4  2005/05/05 05:55:45  rogeeff
//   portability fixes
//
//   Revision 1.3  2005/05/03 05:02:50  rogeeff
//   como fixes
//
//   Revision 1.2  2005/04/27 03:29:52  rogeeff
//   portability fix
//
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_ENV_ENVIRONMENT_IPP_062904GER
