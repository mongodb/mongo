// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_DETAIL_CLOSER_HPP_INCLUDED
#define BOOST_IOSTREAMS_DETAIL_CLOSER_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <exception>                       // exception.
#include <boost/iostreams/detail/ios.hpp>  // openmode.
#include <boost/iostreams/operations.hpp>  // close
#include <boost/iostreams/traits.hpp>      // is_device.
#include <boost/mpl/if.hpp>

namespace boost { namespace iostreams { namespace detail {

template<typename T>
struct closer {
    closer(T& t) : t_(&t) { }
    ~closer() { try { t_->close(); } catch (std::exception&) { } }
    T* t_;
};

template<typename Device>
struct external_device_closer {
    external_device_closer(Device& dev, BOOST_IOS::openmode which)
        : device_(&dev), which_(which),
          dummy_(true), nothrow_(dummy_)
        { }
    external_device_closer(Device& dev, BOOST_IOS::openmode which, bool& nothrow)
        : device_(&dev), which_(which), 
          dummy_(true), nothrow_(nothrow)
        { }
    ~external_device_closer() 
    { 
        try { 
            boost::iostreams::close(*device_, which_); 
        } catch (...) {
            if (!nothrow_) {
                nothrow_ = true;
                throw;
            }
        } 
    }
    Device*               device_;
    BOOST_IOS::openmode   which_;
    bool                  dummy_;
    bool&                 nothrow_;
};

template<typename Filter, typename Device>
struct external_filter_closer {
    external_filter_closer(Filter& flt, Device& dev, BOOST_IOS::openmode which)
        : filter_(flt), device_(dev), which_(which), 
          dummy_(true), nothrow_(dummy_) 
        { }
    external_filter_closer( Filter& flt, Device& dev, 
                            BOOST_IOS::openmode which, bool& nothrow )
        : filter_(flt), device_(dev), which_(which), 
          dummy_(true), nothrow_(nothrow) 
        { }
    ~external_filter_closer() 
    { 
        try { 
            boost::iostreams::close(filter_, device_, which_); 
        } catch (...) { 
            if (!nothrow_) {
                nothrow_ = true;
                throw;
            }
        } 
    }
    Filter&               filter_;
    Device&               device_;
    BOOST_IOS::openmode   which_;
    bool                  dummy_;
    bool&                 nothrow_;
};

template<typename FilterOrDevice, typename DeviceOrDummy = int>
struct external_closer_traits {
    typedef typename 
            mpl::if_<
                is_device<FilterOrDevice>,
                external_device_closer<FilterOrDevice>,
                external_filter_closer<FilterOrDevice, DeviceOrDummy>
            >::type type;
};

template<typename FilterOrDevice, typename DeviceOrDummy = int>
struct external_closer 
    : external_closer_traits<FilterOrDevice, DeviceOrDummy>::type
{ 
    typedef typename 
            external_closer_traits<
                FilterOrDevice, DeviceOrDummy
            >::type base_type;
    external_closer(FilterOrDevice& dev, BOOST_IOS::openmode which)
        : base_type(dev, which)
    { BOOST_STATIC_ASSERT(is_device<FilterOrDevice>::value); };
    external_closer( FilterOrDevice& dev, BOOST_IOS::openmode which,
                     bool& nothrow )
        : base_type(dev, which, nothrow)
    { BOOST_STATIC_ASSERT(is_device<FilterOrDevice>::value); };
    external_closer( FilterOrDevice& flt, DeviceOrDummy& dev,
                     BOOST_IOS::openmode which )
        : base_type(flt, dev, which)
    { BOOST_STATIC_ASSERT(is_filter<FilterOrDevice>::value); };
    external_closer( FilterOrDevice& flt, DeviceOrDummy& dev,
                     BOOST_IOS::openmode which, bool& nothrow )
        : base_type(flt, dev, which, nothrow)
    { BOOST_STATIC_ASSERT(is_filter<FilterOrDevice>::value); };
};

} } } // End namespaces detail, iostreams, boost.

#endif // #ifndef BOOST_IOSTREAMS_DETAIL_CLOSER_HPP_INCLUDED
