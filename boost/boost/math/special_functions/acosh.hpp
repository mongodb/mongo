//    boost asinh.hpp header file

//  (C) Copyright Eric Ford 2001 & Hubert Holin.
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_ACOSH_HPP
#define BOOST_ACOSH_HPP


#include <cmath>
#include <limits>
#include <string>
#include <stdexcept>


#include <boost/config.hpp>


// This is the inverse of the hyperbolic cosine function.

namespace boost
{
    namespace math
    {
#if defined(__GNUC__) && (__GNUC__ < 3)
        // gcc 2.x ignores function scope using declarations,
        // put them in the scope of the enclosing namespace instead:
        
        using    ::std::abs;
        using    ::std::sqrt;
        using    ::std::log;
        
        using    ::std::numeric_limits;
#endif
        
#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
        // This is the main fare
        
        template<typename T>
        inline T    acosh(const T x)
        {
            using    ::std::abs;
            using    ::std::sqrt;
            using    ::std::log;
            
            using    ::std::numeric_limits;
            
            
            T const    one = static_cast<T>(1);
            T const    two = static_cast<T>(2);
            
            static T const    taylor_2_bound = sqrt(numeric_limits<T>::epsilon());
            static T const    taylor_n_bound = sqrt(taylor_2_bound);
            static T const    upper_taylor_2_bound = one/taylor_2_bound;
            
            if        (x < one)
            {
                if    (numeric_limits<T>::has_quiet_NaN)
                {
                    return(numeric_limits<T>::quiet_NaN());
                }
                else
                {
                    ::std::string        error_reporting("Argument to atanh is strictly greater than +1 or strictly smaller than -1!");
                    ::std::domain_error  bad_argument(error_reporting);
                    
                    throw(bad_argument);
                }
            }
            else if    (x >= taylor_n_bound)
            {
                if    (x > upper_taylor_2_bound)
                {
                    // approximation by laurent series in 1/x at 0+ order from -1 to 0
                    return( log( x*two) );
                }
                else
                {
                    return( log( x + sqrt(x*x-one) ) );
                }
            }
            else
            {
                T    y = sqrt(x-one);
                
                // approximation by taylor series in y at 0 up to order 2
                T    result = y;
                
                if    (y >= taylor_2_bound)
                {
                    T    y3 = y*y*y;
                    
                    // approximation by taylor series in y at 0 up to order 4
                    result -= y3/static_cast<T>(12);
                }
                
                return(sqrt(static_cast<T>(2))*result);
            }
        }
#else
        // These are implementation details (for main fare see below)
        
        namespace detail
        {
            template    <
                            typename T,
                            bool QuietNanSupported
                        >
            struct    acosh_helper2_t
            {
                static T    get_NaN()
                {
                    return(::std::numeric_limits<T>::quiet_NaN());
                }
            };  // boost::detail::acosh_helper2_t
            
            
            template<typename T>
            struct    acosh_helper2_t<T, false>
            {
                static T    get_NaN()
                {
                    ::std::string        error_reporting("Argument to acosh is greater than or equal to +1!");
                    ::std::domain_error  bad_argument(error_reporting);
                    
                    throw(bad_argument);
                }
            };  // boost::detail::acosh_helper2_t
        
        }  // boost::detail
        
        
        // This is the main fare
        
        template<typename T>
        inline T    acosh(const T x)
        {
            using    ::std::abs;
            using    ::std::sqrt;
            using    ::std::log;
            
            using    ::std::numeric_limits;
            
            typedef    detail::acosh_helper2_t<T, std::numeric_limits<T>::has_quiet_NaN>    helper2_type;
            
            
            T const    one = static_cast<T>(1);
            T const    two = static_cast<T>(2);
            
            static T const    taylor_2_bound = sqrt(numeric_limits<T>::epsilon());
            static T const    taylor_n_bound = sqrt(taylor_2_bound);
            static T const    upper_taylor_2_bound = one/taylor_2_bound;
            
            if        (x < one)
            {
                return(helper2_type::get_NaN());
            }
            else if    (x >= taylor_n_bound)
            {
                if    (x > upper_taylor_2_bound)
                {
                    // approximation by laurent series in 1/x at 0+ order from -1 to 0
                    return( log( x*two) );
                }
                else
                {
                    return( log( x + sqrt(x*x-one) ) );
                }
            }
            else
            {
                T    y = sqrt(x-one);
                
                // approximation by taylor series in y at 0 up to order 2
                T    result = y;
                
                if    (y >= taylor_2_bound)
                {
                    T    y3 = y*y*y;
                    
                    // approximation by taylor series in y at 0 up to order 4
                    result -= y3/static_cast<T>(12);
                }
                
                return(sqrt(static_cast<T>(2))*result);
            }
        }
#endif /* defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) */
    }
}

#endif /* BOOST_ACOSH_HPP */


