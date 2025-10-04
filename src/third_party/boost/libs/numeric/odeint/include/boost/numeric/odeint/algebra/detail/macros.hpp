/*
 [auto_generated]
 boost/numeric/odeint/algebra/detail/macros.hpp

 [begin_description]
 Some macros for type checking.
 [end_description]

 Copyright 2010-2012 Karsten Ahnert
 Copyright 2010 Mario Mulansky

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
 */


#ifndef BOOST_NUMERIC_ODEINT_ALGEBRA_DETAIL_MACROS_HPP_INCLUDED
#define BOOST_NUMERIC_ODEINT_ALGEBRA_DETAIL_MACROS_HPP_INCLUDED

#include <type_traits>

//type traits aren't working with nvcc
#ifndef __CUDACC__

#define BOOST_ODEINT_CHECK_CONTAINER_TYPE( Type1 , Type2 ) \
        static_assert(( std::is_same< typename std::remove_const< Type1 >::type , Type2 >::value ));

#else
//empty macro for nvcc
#define BOOST_ODEINT_CHECK_CONTAINER_TYPE( Type1 , Type2 )

#endif // __CUDACC__

#endif // BOOST_NUMERIC_ODEINT_ALGEBRA_DETAIL_MACROS_HPP_INCLUDED
