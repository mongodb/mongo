#ifndef BOOST_ARCHIVE_BINARY_OARCHIVE_IMPL_HPP
#define BOOST_ARCHIVE_BINARY_OARCHIVE_IMPL_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// binary_oarchive_impl.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <ostream>
#include <boost/pfto.hpp>
#include <boost/archive/basic_binary_oprimitive.hpp>
#include <boost/archive/basic_binary_oarchive.hpp>

namespace boost { 
namespace archive {

template<class Archive, class Elem, class Tr>
class binary_oarchive_impl : 
    public basic_binary_oprimitive<Archive, Elem, Tr>,
    public basic_binary_oarchive<Archive>
{
#ifdef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
public:
#else
    friend class detail::interface_oarchive<Archive>;
    friend class basic_binary_oarchive<Archive>;
    friend class save_access;
protected:
#endif
    // note: the following should not needed - but one compiler (vc 7.1)
    // fails to compile one test (test_shared_ptr) without it !!!
    // make this protected so it can be called from a derived archive
    template<class T>
    void save_override(T & t, BOOST_PFTO int){
        basic_binary_oarchive<Archive>::save_override(t, 0);
    }
    void init(unsigned int flags) {
        if(0 != (flags & no_header))
            return;
        #if ! defined(__MWERKS__)
            this->basic_binary_oarchive<Archive>::init();
            this->basic_binary_oprimitive<Archive, Elem, Tr>::init();
        #else
            basic_binary_oarchive<Archive>::init();
            basic_binary_oprimitive<Archive, Elem, Tr>::init();
        #endif
    }
    binary_oarchive_impl(
        std::basic_streambuf<Elem, Tr> & bsb, 
        unsigned int flags
    ) :
        basic_binary_oprimitive<Archive, Elem, Tr>(
            bsb, 
            0 != (flags & no_codecvt)
        ),
        basic_binary_oarchive<Archive>(flags)
    {
        init(flags);
    }
    binary_oarchive_impl(
        std::basic_ostream<Elem, Tr> & os, 
        unsigned int flags
    ) :
        basic_binary_oprimitive<Archive, Elem, Tr>(
            * os.rdbuf(), 
            0 != (flags & no_codecvt)
        ),
        basic_binary_oarchive<Archive>(flags)
    {
        init(flags);
    }
};

} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_BINARY_OARCHIVE_IMPL_HPP
