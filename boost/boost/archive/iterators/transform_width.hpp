#ifndef BOOST_ARCHIVE_ITERATORS_TRANSFORM_WIDTH_HPP
#define BOOST_ARCHIVE_ITERATORS_TRANSFORM_WIDTH_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// transform_width.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// iterator which takes elements of x bits and returns elements of y bits.
// used to change streams of 8 bit characters into streams of 6 bit characters.
// and vice-versa for implementing base64 encodeing/decoding. Be very careful
// when using and end iterator.  end is only reliable detected when the input
// stream length is some common multiple of x and y.  E.G. Base64 6 bit
// character and 8 bit bytes. Lowest common multiple is 24 => 4 6 bit characters
// or 3 8 bit characters

#include <boost/config.hpp> // for BOOST_DEDUCED_TYPENAME & PTFO
#include <boost/pfto.hpp>

#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/iterator_traits.hpp>

namespace boost { 
namespace archive {
namespace iterators {

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// class used by text archives to translate char strings to wchar_t
// strings of the currently selected locale
template<
    class Base, 
    int BitsOut, 
    int BitsIn, 
    class CharType = BOOST_DEDUCED_TYPENAME boost::iterator_value<Base>::type // output character
>
class transform_width : 
    public boost::iterator_adaptor<
        transform_width<Base, BitsOut, BitsIn, CharType>,
        Base,
        CharType,
        single_pass_traversal_tag,
        CharType
    >
{
    friend class boost::iterator_core_access;
    typedef BOOST_DEDUCED_TYPENAME boost::iterator_adaptor<
        transform_width<Base, BitsOut, BitsIn, CharType>,
        Base,
        CharType,
        single_pass_traversal_tag,
        CharType
    > super_t;

    typedef transform_width<Base, BitsOut, BitsIn, CharType> this_t;
    typedef BOOST_DEDUCED_TYPENAME iterator_value<Base>::type base_value_type;

    CharType fill();

    CharType dereference_impl(){
        if(! m_full){
            m_current_value = fill();
            m_full = true;
        }
        return m_current_value;
    }

    CharType dereference() const {
        return const_cast<this_t *>(this)->dereference_impl();
    }

    // test for iterator equality
    bool equal(const this_t & rhs) const {
        return
            this->base_reference() == rhs.base_reference();
        ;
    }

    void increment(){
        m_displacement += BitsOut;

        while(m_displacement >= BitsIn){
            m_displacement -= BitsIn;
            if(0 == m_displacement)
                m_bufferfull = false;
            if(! m_bufferfull){
                // note: suspect that this is not invoked for borland
                ++(this->base_reference());
            }
        }
        m_full = false;
    }

    CharType m_current_value;
    // number of bits left in current input character buffer
    unsigned int m_displacement;
    base_value_type m_buffer;
    // flag to current output character is ready - just used to save time
    bool m_full;
    // flag to indicate that m_buffer has data
    bool m_bufferfull;

public:
    // make composible buy using templated constructor
    template<class T>
    transform_width(BOOST_PFTO_WRAPPER(T) start) : 
        super_t(Base(BOOST_MAKE_PFTO_WRAPPER(static_cast<T>(start)))),
        m_displacement(0),
        m_full(false),
        m_bufferfull(false)
    {}
    // intel 7.1 doesn't like default copy constructor
    transform_width(const transform_width & rhs) : 
        super_t(rhs.base_reference()),
        m_current_value(rhs.m_current_value),
        m_displacement(rhs.m_displacement),
        m_buffer(rhs.m_buffer),
        m_full(rhs.m_full),
        m_bufferfull(rhs.m_bufferfull)
    {}
};

template<class Base, int BitsOut, int BitsIn, class CharType>
CharType transform_width<Base, BitsOut, BitsIn, CharType>::fill(){
    CharType retval = 0;
    unsigned int missing_bits = BitsOut;
    for(;;){
        unsigned int bcount;
        if(! m_bufferfull){
            m_buffer = * this->base_reference();
            m_bufferfull = true;
            bcount = BitsIn;
        }
        else
            bcount = BitsIn - m_displacement;
        unsigned int i = (std::min)(bcount, missing_bits);
        // shift interesting bits to least significant position
        unsigned int j = m_buffer >> (bcount - i);
        // strip off uninteresting bits
        // (note presumption of two's complement arithmetic)
        j &= ~(-(1 << i));
        // append then interesting bits to the output value
        retval <<= i;
        retval |= j;
        missing_bits -= i;
        if(0 == missing_bits)
            break;
        // note: suspect that this is not invoked for borland 5.51
        ++(this->base_reference());
        m_bufferfull = false;
    }
    return retval;
}

} // namespace iterators
} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_ITERATORS_TRANSFORM_WIDTH_HPP
