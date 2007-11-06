// (C) Copyright Jonathan Turkanis 2005.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_DETAIL_DEFAULT_ARG_HPP_INCLUDED
#define BOOST_IOSTREAMS_DETAIL_DEFAULT_ARG_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif            

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
# include <boost/mpl/identity.hpp>
# define BOOST_IOSTREAMS_DEFAULT_ARG(arg) mpl::identity< arg >::type
#else
# define BOOST_IOSTREAMS_DEFAULT_ARG(arg) arg
#endif

#endif // #ifndef BOOST_IOSTREAMS_DETAIL_DEFAULT_ARG_HPP_INCLUDED
