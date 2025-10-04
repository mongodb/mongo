/* boost random/detail/config.hpp header file
 *
 * Copyright Steven Watanabe 2009
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id$
 */

#include <boost/config.hpp>

#if (defined(BOOST_NO_OPERATORS_IN_NAMESPACE) || defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)) \
    && !defined(BOOST_MSVC)
    #define BOOST_RANDOM_NO_STREAM_OPERATORS
#endif

#if ((defined(__cplusplus) && __cplusplus >= 201703L) || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)) && (defined(__cpp_hex_float) && __cpp_hex_float >= 201603L)
#  define BOOST_RANDOM_HAS_HEX_FLOAT
#endif
