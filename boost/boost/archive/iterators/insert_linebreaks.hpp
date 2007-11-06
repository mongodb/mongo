#ifndef BOOST_ARCHIVE_ITERATORS_INSERT_LINEBREAKS_HPP
#define BOOST_ARCHIVE_ITERATORS_INSERT_LINEBREAKS_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// binary_from_base64.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cassert>

#include <boost/config.hpp> // for BOOST_DEDUCED_TYPENAME
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ using ::memcpy; }
#endif

#include <boost/pfto.hpp>

#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/iterator_traits.hpp>

namespace boost { 
namespace archive {
namespace iterators {

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// insert line break every N characters
template<
    class Base, 
    int N, 
    class CharType = BOOST_DEDUCED_TYPENAME boost::iterator_value<Base>::type
>
class insert_linebreaks : 
    public iterator_adaptor<
        insert_linebreaks<Base, N, CharType>,
        Base,
        CharType,
        single_pass_traversal_tag,
        CharType
    >
{
private:
    friend class boost::iterator_core_access;
    typedef iterator_adaptor<
        insert_linebreaks<Base, N, CharType>,
        Base,
        CharType,
        single_pass_traversal_tag,
        CharType
    > super_t;

    bool equal(const insert_linebreaks<Base, N, CharType> & rhs) const {
        return
//            m_count == rhs.m_count
//            && base_reference() == rhs.base_reference()
            this->base_reference() == rhs.base_reference()
        ;
    }

    void increment() {
        if(m_count == N){
            m_count = 0;
            return;
        }
        ++m_count;
        ++(this->base_reference());
    }
    CharType dereference() const {
        if(m_count == N)
            return '\n';
        return * (this->base_reference());
    }
    unsigned int m_count;
public:
    // make composible buy using templated constructor
    template<class T>
    insert_linebreaks(BOOST_PFTO_WRAPPER(T)  start) :
        super_t(Base(BOOST_MAKE_PFTO_WRAPPER(static_cast<T>(start)))),
        m_count(0)
    {}
    // intel 7.1 doesn't like default copy constructor
    insert_linebreaks(const insert_linebreaks & rhs) : 
        super_t(rhs.base_reference()),
        m_count(rhs.m_count)
    {}
};

} // namespace iterators
} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_ITERATORS_INSERT_LINEBREAKS_HPP
