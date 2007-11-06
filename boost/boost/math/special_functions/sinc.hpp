//  boost sinc.hpp header file

//  (C) Copyright Hubert Holin 2001.
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_SINC_HPP
#define BOOST_SINC_HPP


#include <cmath>
#include <boost/limits.hpp>
#include <string>
#include <stdexcept>


#include <boost/config.hpp>


// These are the the "Sinus Cardinal" functions.

namespace boost
{
    namespace math
    {
#if        defined(__GNUC__) && (__GNUC__ < 3)
        // gcc 2.x ignores function scope using declarations,
        // put them in the scope of the enclosing namespace instead:

        using    ::std::abs;
        using    ::std::sqrt;
        using    ::std::sin;

        using    ::std::numeric_limits;
#endif    /* defined(__GNUC__) && (__GNUC__ < 3) */

        // This is the "Sinus Cardinal" of index Pi.

        template<typename T>
        inline T    sinc_pi(const T x)
        {
#ifdef    BOOST_NO_STDC_NAMESPACE
            using    ::abs;
            using    ::sin;
            using    ::sqrt;
#else    /* BOOST_NO_STDC_NAMESPACE */
            using    ::std::abs;
            using    ::std::sin;
            using    ::std::sqrt;
#endif    /* BOOST_NO_STDC_NAMESPACE */

            using    ::std::numeric_limits;

            static T const    taylor_0_bound = numeric_limits<T>::epsilon();
            static T const    taylor_2_bound = sqrt(taylor_0_bound);
            static T const    taylor_n_bound = sqrt(taylor_2_bound);

            if    (abs(x) >= taylor_n_bound)
            {
                return(sin(x)/x);
            }
            else
            {
                // approximation by taylor series in x at 0 up to order 0
                T    result = static_cast<T>(1);

                if    (abs(x) >= taylor_0_bound)
                {
                    T    x2 = x*x;

                    // approximation by taylor series in x at 0 up to order 2
                    result -= x2/static_cast<T>(6);

                    if    (abs(x) >= taylor_2_bound)
                    {
                        // approximation by taylor series in x at 0 up to order 4
                        result += (x2*x2)/static_cast<T>(120);
                    }
                }

                return(result);
            }
        }


#ifdef    BOOST_NO_TEMPLATE_TEMPLATES
#else    /* BOOST_NO_TEMPLATE_TEMPLATES */
        template<typename T, template<typename> class U>
        inline U<T>    sinc_pi(const U<T> x)
        {
#if defined(BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL) || defined(__GNUC__)
            using namespace std;
#elif    defined(BOOST_NO_STDC_NAMESPACE)
            using    ::abs;
            using    ::sin;
            using    ::sqrt;
#else    /* BOOST_NO_STDC_NAMESPACE */
            using    ::std::abs;
            using    ::std::sin;
            using    ::std::sqrt;
#endif    /* BOOST_NO_STDC_NAMESPACE */

            using    ::std::numeric_limits;

            static T const    taylor_0_bound = numeric_limits<T>::epsilon();
            static T const    taylor_2_bound = sqrt(taylor_0_bound);
            static T const    taylor_n_bound = sqrt(taylor_2_bound);

            if    (abs(x) >= taylor_n_bound)
            {
                return(sin(x)/x);
            }
            else
            {
                // approximation by taylor series in x at 0 up to order 0
#ifdef __MWERKS__
                U<T>    result = static_cast<U<T> >(1);
#else
                U<T>    result = U<T>(1);
#endif

                if    (abs(x) >= taylor_0_bound)
                {
                    U<T>    x2 = x*x;

                    // approximation by taylor series in x at 0 up to order 2
                    result -= x2/static_cast<T>(6);

                    if    (abs(x) >= taylor_2_bound)
                    {
                        // approximation by taylor series in x at 0 up to order 4
                        result += (x2*x2)/static_cast<T>(120);
                    }
                }

                return(result);
            }
        }
#endif    /* BOOST_NO_TEMPLATE_TEMPLATES */
    }
}

#endif /* BOOST_SINC_HPP */
