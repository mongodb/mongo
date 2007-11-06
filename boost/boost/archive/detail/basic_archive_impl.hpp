#ifndef BOOST_ARCHIVE_DETAIL_BASIC_ARCHIVE_IMPL_HPP
#define BOOST_ARCHIVE_DETAIL_BASIC_ARCHIVE_IMPL_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_archive_impl.hpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// can't use this - much as I'd like to as borland doesn't support it
// #include <boost/scoped_ptr.hpp>

#include <set>
#include <boost/shared_ptr.hpp>

#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

namespace boost {
namespace serialization {
    class extended_type_info;
} // namespace serialization

namespace archive {
namespace detail {

//////////////////////////////////////////////////////////////////////
class BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) basic_archive_impl
{
    //////////////////////////////////////////////////////////////////////
    // list of serialization helpers
    // at least one compiler sunpro 5.3 erroneously doesn't give access to embedded structs
    struct helper_compare;
    friend struct helper_compare;

    struct helper_type {
        shared_ptr<void> m_helper;
        const boost::serialization::extended_type_info * m_eti;
        helper_type(
            shared_ptr<void> h, 
            const boost::serialization::extended_type_info * const eti
        ) :
            m_helper(h),
            m_eti(eti)
        {}
    };

    struct helper_compare {
        bool operator()(
            const helper_type & lhs, 
            const helper_type & rhs
        ) const {
            return lhs.m_eti < rhs.m_eti;
        }
    };

    typedef std::set<helper_type, helper_compare> collection;
    typedef collection::iterator helper_iterator;
    typedef collection::const_iterator helper_const_iterator;
    collection m_helpers;
protected:
    void
    lookup_helper(
        const boost::serialization::extended_type_info * const eti,
        shared_ptr<void> & sph
    );
    void
    insert_helper(
        const boost::serialization::extended_type_info * const eti,
        shared_ptr<void> & sph
    );
};

} // namespace detail
} // namespace serialization
} // namespace boost

#include <boost/archive/detail/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif //BOOST_ARCHIVE_DETAIL_BASIC_ARCHIVE_IMPL_HPP



