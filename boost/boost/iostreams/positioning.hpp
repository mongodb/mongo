// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

// Thanks to Gareth Sylvester-Bradley for the Dinkumware versions of the
// positioning functions.

#ifndef BOOST_IOSTREAMS_POSITIONING_HPP_INCLUDED
#define BOOST_IOSTREAMS_POSITIONING_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/integer_traits.hpp>
#include <boost/iostreams/detail/config/codecvt.hpp> // mbstate_t.
#include <boost/iostreams/detail/ios.hpp> // streamoff, streampos.

// Must come last.
#include <boost/iostreams/detail/config/disable_warnings.hpp> 

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::fpos_t; }
#endif

namespace boost { namespace iostreams {

typedef boost::intmax_t stream_offset;

inline std::streamoff stream_offset_to_streamoff(stream_offset off)
{ return static_cast<stream_offset>(off); }

template<typename PosType> // Hande custom pos_type's.
inline stream_offset position_to_offset(PosType pos)
{ return std::streamoff(pos); }

#if ((defined(_YVALS) && !defined(__IBMCPP__)) || defined(_CPPLIB_VER)) && \
     !defined(__SGI_STL_PORT) && !defined(_STLPORT_VERSION) \
     && !defined(__QNX__) \
   /**/

        /* Dinkumware */

inline std::streampos offset_to_position(stream_offset off)
{
    // Use implementation-specific constructor.
    return std::streampos(std::mbstate_t(), off);
}

inline stream_offset fpos_t_to_offset(std::fpos_t pos)
{ // Helper function.
#if defined(_POSIX_) || (_INTEGRAL_MAX_BITS >= 64)
    return pos;
#else
    return _FPOSOFF(pos);
#endif
}

# if defined(_CPPLIB_VER) //--------------------------------------------------//

        /* Recent Dinkumware */

inline stream_offset position_to_offset(std::streampos pos)
{
    // Use implementation-specific member function seekpos().
    return fpos_t_to_offset(pos.seekpos()) +
           stream_offset(std::streamoff(pos)) -
           stream_offset(std::streamoff(pos.seekpos()));
}

# else // # if defined(_CPPLIB_VER) //----------------------------------------//

        /* Old Dinkumware */

inline stream_offset position_to_offset(std::streampos pos)
{
    // use implementation-specific member function get_fpos_t().
    return fpos_t_to_offset(pos.get_fpos_t()) +
           stream_offset(std::streamoff(pos)) -
           stream_offset(std::streamoff(pos.get_fpos_t()));
}

# endif // # if defined(_CPPLIB_VER) //---------------------------------------//
#else // Dinkumware //--------------------------------------------------------//

        /* Non-Dinkumware */

inline std::streampos offset_to_position(stream_offset off) { return off; }

inline stream_offset position_to_offset(std::streampos pos) { return pos; }

#endif // Dinkumware //-------------------------------------------------------//

} } // End namespaces iostreams, boost.

#include <boost/iostreams/detail/config/enable_warnings.hpp> 

#endif // #ifndef BOOST_IOSTREAMS_POSITIONING_HPP_INCLUDED
