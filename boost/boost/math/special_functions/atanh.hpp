//    boost atanh.hpp header file

//  (C) Copyright Hubert Holin 2001.
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_ATANH_HPP
#define BOOST_ATANH_HPP


#include <cmath>
#include <limits>
#include <string>
#include <stdexcept>


#include <boost/config.hpp>


// This is the inverse of the hyperbolic tangent function.

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
        inline T    atanh(const T x)
        {
            using    ::std::abs;
            using    ::std::sqrt;
            using    ::std::log;
            
            using    ::std::numeric_limits;
            
            T const            one = static_cast<T>(1);
            T const            two = static_cast<T>(2);
            
            static T const    taylor_2_bound = sqrt(numeric_limits<T>::epsilon());
            static T const    taylor_n_bound = sqrt(taylor_2_bound);
            
            if        (x < -one)
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
            else if    (x < -one+numeric_limits<T>::epsilon())
            {
                if    (numeric_limits<T>::has_infinity)
                {
                    return(-numeric_limits<T>::infinity());
                }
                else
                {
                    ::std::string        error_reporting("Argument to atanh is -1 (result: -Infinity)!");
                    ::std::out_of_range  bad_argument(error_reporting);
                    
                    throw(bad_argument);
                }
            }
            else if    (x > +one-numeric_limits<T>::epsilon())
            {
                if    (numeric_limits<T>::has_infinity)
                {
                    return(+numeric_limits<T>::infinity());
                }
                else
                {
                    ::std::string        error_reporting("Argument to atanh is +1 (result: +Infinity)!");
                    ::std::out_of_range  bad_argument(error_reporting);
                    
                    throw(bad_argument);
                }
            }
            else if    (x > +one)
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
            else if    (abs(x) >= taylor_n_bound)
            {
                return(log( (one + x) / (one - x) ) / two);
            }
            else
            {
                // approximation by taylor series in x at 0 up to order 2
                T    result = x;
                
                if    (abs(x) >= taylor_2_bound)
                {
                    T    x3 = x*x*x;
                    
                    // approximation by taylor series in x at 0 up to order 4
                    result += x3/static_cast<T>(3);
                }
                
                return(result);
            }
        }
#else
        // These are implementation details (for main fare see below)
        
        namespace detail
        {
            template    <
                            typename T,
                            bool InfinitySupported
                        >
            struct    atanh_helper1_t
            {
                static T    get_pos_infinity()
                {
                    return(+::std::numeric_limits<T>::infinity());
                }
                
                static T    get_neg_infinity()
                {
                    return(-::std::numeric_limits<T>::infinity());
                }
            };    // boost::math::detail::atanh_helper1_t
            
            
            template<typename T>
            struct    atanh_helper1_t<T, false>
            {
                static T    get_pos_infinity()
                {
                    ::std::string        error_reporting("Argument to atanh is +1 (result: +Infinity)!");
                    ::std::out_of_range  bad_argument(error_reporting);
                    
                    throw(bad_argument);
                }
                
                static T    get_neg_infinity()
                {
                    ::std::string        error_reporting("Argument to atanh is -1 (result: -Infinity)!");
                    ::std::out_of_range  bad_argument(error_reporting);
                    
                    throw(bad_argument);
                }
            };    // boost::math::detail::atanh_helper1_t
            
            
            template    <
                            typename T,
                            bool QuietNanSupported
                        >
            struct    atanh_helper2_t
            {
                static T    get_NaN()
                {
                    return(::std::numeric_limits<T>::quiet_NaN());
                }
            };    // boost::detail::atanh_helper2_t
            
            
            template<typename T>
            struct    atanh_helper2_t<T, false>
            {
                static T    get_NaN()
                {
                    ::std::string        error_reporting("Argument to atanh is strictly greater than +1 or strictly smaller than -1!");
                    ::std::domain_error  bad_argument(error_reporting);
                    
                    throw(bad_argument);
                }
            };    // boost::detail::atanh_helper2_t
        }    // boost::detail
        
        
        // This is the main fare
        
        template<typename T>
        inline T    atanh(const T x)
        {
            using    ::std::abs;
            using    ::std::sqrt;
            using    ::std::log;
            
            using    ::std::numeric_limits;
            
            typedef  detail::atanh_helper1_t<T, ::std::numeric_limits<T>::has_infinity>    helper1_type;
            typedef  detail::atanh_helper2_t<T, ::std::numeric_limits<T>::has_quiet_NaN>    helper2_type;
            
            
            T const           one = static_cast<T>(1);
            T const           two = static_cast<T>(2);
            
            static T const    taylor_2_bound = sqrt(numeric_limits<T>::epsilon());
            static T const    taylor_n_bound = sqrt(taylor_2_bound);
            
            if        (x < -one)
            {
                return(helper2_type::get_NaN());
            }
            else if    (x < -one+numeric_limits<T>::epsilon())
            {
                return(helper1_type::get_neg_infinity());
            }
            else if    (x > +one-numeric_limits<T>::epsilon())
            {
                return(helper1_type::get_pos_infinity());
            }
            else if    (x > +one)
            {
                return(helper2_type::get_NaN());
            }
            else if    (abs(x) >= taylor_n_bound)
            {
                return(log( (one + x) / (one - x) ) / two);
            }
            else
            {
                // approximation by taylor series in x at 0 up to order 2
                T    result = x;
                
                if    (abs(x) >= taylor_2_bound)
                {
                    T    x3 = x*x*x;
                    
                    // approximation by taylor series in x at 0 up to order 4
                    result += x3/static_cast<T>(3);
                }
                
                return(result);
            }
        }
#endif /* defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) */
    }
}

#endif /* BOOST_ATANH_HPP */

