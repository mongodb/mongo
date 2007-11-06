// (C) Copyright Jonathan Turkanis 2005.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_DETAIL_BASIC_ADAPTER_HPP_INCLUDED
#define BOOST_IOSTREAMS_DETAIL_BASIC_ADAPTER_HPP_INCLUDED

#include <boost/iostreams/categories.hpp>
#include <boost/iostreams/detail/call_traits.hpp>
#include <boost/iostreams/detail/ios.hpp>
#include <boost/iostreams/operations.hpp>
#include <boost/iostreams/traits.hpp>
#include <boost/static_assert.hpp>

namespace boost { namespace iostreams { namespace detail {

template<typename T>
class basic_adapter {
private:
    typedef typename detail::value_type<T>::type value_type;
    typedef typename detail::param_type<T>::type param_type;
public:
    explicit basic_adapter(param_type t) : t_(t) { }
    T& component() { return t_; }

    void close(BOOST_IOS::openmode which = BOOST_IOS::in | BOOST_IOS::out) 
    { 
        BOOST_STATIC_ASSERT(is_device<T>::value);
        iostreams::close(t_, which); 
    }

    template<typename Device>
    void close( Device& dev, 
                BOOST_IOS::openmode which = 
                    BOOST_IOS::in | BOOST_IOS::out ) 
    { 
        BOOST_STATIC_ASSERT(is_filter<T>::value);
        iostreams::close(t_, dev, which); 
    }

    bool flush() 
    { 
        BOOST_STATIC_ASSERT(is_device<T>::value);
        return iostreams::flush(t_); 
    }

    template<typename Device>
    void flush(Device& dev) 
    { 
        BOOST_STATIC_ASSERT(is_filter<T>::value);
        return iostreams::flush(t_, dev); 
    }

    template<typename Locale> // Avoid dependency on <locale>
    void imbue(const Locale& loc) { iostreams::imbue(t_, loc); }

    std::streamsize optimal_buffer_size() const 
    { return iostreams::optimal_buffer_size(t_); }
public:
    value_type t_;
};

//----------------------------------------------------------------------------//

} } } // End namespaces detail, iostreams, boost.

#endif // #ifndef BOOST_IOSTREAMS_DETAIL_BASIC_ADAPTER_HPP_INCLUDED
