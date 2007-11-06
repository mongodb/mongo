#ifndef BOOST_ARCHIVE_BINARY_IARCHIVE_IMPL_HPP
#define BOOST_ARCHIVE_BINARY_IARCHIVE_IMPL_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// binary_iarchive_impl.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <istream>
#include <boost/pfto.hpp>
#include <boost/archive/basic_binary_iprimitive.hpp>
#include <boost/archive/basic_binary_iarchive.hpp>

namespace boost { 
namespace archive {

template<class Archive, class Elem, class Tr>
class binary_iarchive_impl : 
    public basic_binary_iprimitive<Archive, Elem, Tr>,
    public basic_binary_iarchive<Archive>
{
#ifdef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
public:
#else
    friend class detail::interface_iarchive<Archive>;
    friend class basic_binary_iarchive<Archive>;
    friend class load_access;
protected:
#endif
    // note: the following should not needed - but one compiler (vc 7.1)
    // fails to compile one test (test_shared_ptr) without it !!!
    // make this protected so it can be called from a derived archive
    template<class T>
    void load_override(T & t, BOOST_PFTO int){
        basic_binary_iarchive<Archive>::load_override(t, 0);
    }
    void init(unsigned int flags){
        if(0 != (flags & no_header))
            return;
        #if ! defined(__MWERKS__)
            this->basic_binary_iarchive<Archive>::init();
            this->basic_binary_iprimitive<Archive, Elem, Tr>::init();
        #else
            basic_binary_iarchive<Archive>::init();
            basic_binary_iprimitive<Archive, Elem, Tr>::init();
        #endif
    }
    binary_iarchive_impl(
        std::basic_streambuf<Elem, Tr> & bsb, 
        unsigned int flags
    ) :
        basic_binary_iprimitive<Archive, Elem, Tr>(
            bsb, 
            0 != (flags & no_codecvt)
        ),
        basic_binary_iarchive<Archive>(flags)
    {
        init(flags);
    }
    binary_iarchive_impl(
        std::basic_istream<Elem, Tr> & is, 
        unsigned int flags
    ) :
        basic_binary_iprimitive<Archive, Elem, Tr>(
            * is.rdbuf(), 
            0 != (flags & no_codecvt)
        ),
        basic_binary_iarchive<Archive>(flags)
    {
        init(flags);
    }
};

} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_BINARY_IARCHIVE_IMPL_HPP
