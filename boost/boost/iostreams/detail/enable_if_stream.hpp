// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_DETAIL_ENABLE_IF_STREAM_HPP_INCLUDED
#define BOOST_IOSTREAMS_DETAIL_ENABLE_IF_STREAM_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif              

#include <boost/config.hpp>                // BOOST_NO_SFINAE.
#include <boost/utility/enable_if.hpp>                  
#include <boost/iostreams/traits_fwd.hpp>  // is_std_io.

#ifndef BOOST_NO_SFINAE
# define BOOST_IOSTREAMS_ENABLE_IF_STREAM(T) \
    , typename boost::enable_if< boost::iostreams::is_std_io<T> >::type* = 0  \
    /**/
# define BOOST_IOSTREAMS_DISABLE_IF_STREAM(T) \
    , typename boost::disable_if< boost::iostreams::is_std_io<T> >::type* = 0 \
    /**/
#else 
# define BOOST_IOSTREAMS_ENABLE_IF_STREAM(T)
# define BOOST_IOSTREAMS_DISABLE_IF_STREAM(T)
#endif

#endif // #ifndef BOOST_IOSTREAMS_DETAIL_ENABLE_IF_STREAM_HPP_INCLUDED
