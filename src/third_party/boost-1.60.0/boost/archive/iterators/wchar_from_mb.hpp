#ifndef BOOST_ARCHIVE_ITERATORS_WCHAR_FROM_MB_HPP
#define BOOST_ARCHIVE_ITERATORS_WCHAR_FROM_MB_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// wchar_from_mb.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/assert.hpp>
#include <cctype>
#include <cstddef> // size_t
#include <cwchar> // mbstate_t and mbrtowc

#include <boost/config.hpp>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ 
    using ::mbstate_t;
    using ::mbrtowc;
} // namespace std
#endif

#include <boost/serialization/throw_exception.hpp>

#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/archive/iterators/dataflow_exception.hpp>

namespace boost { 
namespace archive {
namespace iterators {

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// class used by text archives to translate char strings to wchar_t
// strings of the currently selected locale
template<class Base>
class wchar_from_mb 
    : public boost::iterator_adaptor<
        wchar_from_mb<Base>, 
        Base, 
        wchar_t,
        single_pass_traversal_tag,
        wchar_t
    >
{
    friend class boost::iterator_core_access;
    typedef typename boost::iterator_adaptor<
        wchar_from_mb<Base>, 
        Base, 
        wchar_t,
        single_pass_traversal_tag,
        wchar_t
    > super_t;

    typedef wchar_from_mb<Base> this_t;

    wchar_t drain();

    wchar_t dereference_impl() {
        if(! m_full){
            m_current_value = drain();
            m_full = true;
        }
        return m_current_value;
    }

    wchar_t dereference() const {
        return const_cast<this_t *>(this)->dereference_impl();
    }

    void increment(){
        dereference_impl();
        m_full = false;
        ++(this->base_reference());
    };

    wchar_t m_current_value;
    bool m_full;

public:
    // make composible buy using templated constructor
    template<class T>
    wchar_from_mb(T start) : 
        super_t(Base(static_cast< T >(start))),
        m_full(false)
    {}
    // intel 7.1 doesn't like default copy constructor
    wchar_from_mb(const wchar_from_mb & rhs) : 
        super_t(rhs.base_reference()),
        m_full(rhs.m_full)
    {}
};

template<class Base>
wchar_t wchar_from_mb<Base>::drain(){
    std::mbstate_t mbs;
    wchar_t retval;
    std::size_t result;
    do {
        char val = *this->base_reference();
        result = std::mbrtowc(&retval, &val, 1, &mbs);
        if(result == static_cast<std::size_t>(-1))
            boost::serialization::throw_exception(iterators::dataflow_exception(
                iterators::dataflow_exception::invalid_conversion
            ));
    } while (result == static_cast<std::size_t>(-2));
    return retval;
}

} // namespace iterators
} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_ITERATORS_WCHAR_FROM_MB_HPP
