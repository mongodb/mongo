#ifndef  BOOST_ARCHIVE_BASIC_SERIALIZER_HPP
#define BOOST_ARCHIVE_BASIC_SERIALIZER_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_serializer.hpp: extenstion of type_info required for serialization.

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cassert>

#include <boost/noncopyable.hpp>
#include <boost/config.hpp>
#include <boost/serialization/extended_type_info.hpp>

namespace boost {
namespace archive {
namespace detail {

class basic_serializer : private boost::noncopyable
{
    const boost::serialization::extended_type_info & m_eti;
protected:
    explicit basic_serializer(
        const boost::serialization::extended_type_info & eti
    ) : 
        m_eti(eti)
    {}
public:
    const boost::serialization::extended_type_info & get_eti() const {
        return m_eti;
    }
    bool operator<(const basic_serializer & rhs) const {
        return & m_eti < & rhs.get_eti();
    }
};

} // namespace detail
} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_BASIC_SERIALIZER_HPP
