#ifndef BOOST_THROW_EXCEPTION_HPP_INCLUDED
#define BOOST_THROW_EXCEPTION_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//
//  boost/throw_exception.hpp
//
//  Copyright (c) 2002 Peter Dimov and Multi Media Ltd.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
//  http://www.boost.org/libs/utility/throw_exception.html
//

#include <boost/config.hpp>

#ifdef BOOST_NO_EXCEPTIONS
# include <exception>
#endif

namespace boost
{

#ifdef BOOST_NO_EXCEPTIONS

void throw_exception(std::exception const & e); // user defined

#else

template<class E> inline void throw_exception(E const & e)
{
    throw e;
}

#endif

} // namespace boost

#endif // #ifndef BOOST_THROW_EXCEPTION_HPP_INCLUDED
