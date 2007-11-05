// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

// To do: handle bidirection streams and output-seekable components.

#ifndef BOOST_IOSTREAMS_SKIP_HPP_INCLUDED
#define BOOST_IOSTREAMS_SKIP_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/iostreams/char_traits.hpp>
#include <boost/iostreams/detail/ios.hpp>  // failure.
#include <boost/iostreams/operations.hpp>
#include <boost/iostreams/traits.hpp>
#include <boost/mpl/and.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/type_traits/is_convertible.hpp>

namespace boost { namespace iostreams {

namespace detail {

template<typename Device>
void skip(Device& dev, stream_offset off, mpl::true_)
{ iostreams::seek(dev, off, BOOST_IOS::cur); }

template<typename Device>
void skip(Device& dev, stream_offset off, mpl::false_)
{   // gcc 2.95 needs namespace qualification for char_traits.
    typedef typename char_type_of<Device>::type  char_type;
    typedef iostreams::char_traits<char_type>    traits_type;
    for (stream_offset z = 0; z < off; ) {
        typename traits_type::int_type c;
        if (traits_type::is_eof(c = iostreams::get(dev)))
            throw BOOST_IOSTREAMS_FAILURE("bad skip offset");
        if (!traits_type::would_block(c))
            ++z;
    }
}

template<typename Filter, typename Device>
void skip(Filter& flt, Device& dev, stream_offset off, mpl::true_)
{ flt.seek(dev, off, BOOST_IOS::cur); }

template<typename Filter, typename Device>
void skip(Filter& flt, Device& dev, stream_offset off, mpl::false_)
{ 
    typedef typename char_type_of<Device>::type char_type;
    char_type c;
    for (stream_offset z = 0; z < off; ) {
        std::streamsize amt;
        if ((amt = iostreams::read(flt, dev, &c, 1)) == -1)
            throw BOOST_IOSTREAMS_FAILURE("bad skip offset");
        if (amt == 1)
            ++z;
    }
}

} // End namespace detail.

template<typename Device>
void skip(Device& dev, stream_offset off)
{ 
    typedef typename mode_of<Device>::type mode;
    detail::skip(dev, off, is_convertible<mode, seekable>());
}

template<typename Filter, typename Device>
void skip(Filter& flt, Device& dev, stream_offset off)
{ 
    typedef typename mode_of<Filter>::type                     filter_mode;
    typedef typename mode_of<Device>::type                     device_mode;
    typedef mpl::and_<
                is_convertible<filter_mode, output_seekable>,
                is_convertible<device_mode, output_seekable>
            >                                                  can_seek;
    detail::skip(flt, dev, off, can_seek());
}

} } // End namespaces iostreams, boost.

#endif // #ifndef BOOST_IOSTREAMS_SKIP_HPP_INCLUDED //------------------------//
