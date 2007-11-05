//
//  boost/assert.hpp - BOOST_ASSERT(expr)
//
//  Copyright (c) 2001, 2002 Peter Dimov and Multi Media Ltd.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
//  Note: There are no include guards. This is intentional.
//
//  See http://www.boost.org/libs/utility/assert.html for documentation.
//

#undef BOOST_ASSERT

#if defined(BOOST_DISABLE_ASSERTS)

# define BOOST_ASSERT(expr) ((void)0)

#elif defined(BOOST_ENABLE_ASSERT_HANDLER)

#include <boost/current_function.hpp>

namespace boost
{

void assertion_failed(char const * expr, char const * function, char const * file, long line); // user defined

} // namespace boost

#define BOOST_ASSERT(expr) ((expr)? ((void)0): ::boost::assertion_failed(#expr, BOOST_CURRENT_FUNCTION, __FILE__, __LINE__))

#else
# include <assert.h> // .h to support old libraries w/o <cassert> - effect is the same
# define BOOST_ASSERT(expr) assert(expr)
#endif
