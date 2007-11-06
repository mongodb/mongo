#ifndef  BOOST_TYPEINFO_EXTENDED_MAP_HPP
#define BOOST_TYPEINFO_EXTENDED_MAP_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_serializer_map.hpp: extenstion of type_info required for serialization.

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <set>

#include <boost/config.hpp>
#include <boost/utility.hpp>
#include <boost/archive/detail/auto_link_archive.hpp>

#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

namespace boost { 
namespace serialization {
    class extended_type_info;
}

namespace archive {
namespace detail  {

class BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) basic_serializer;

struct BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) type_info_pointer_compare
{
    bool operator()(
        const basic_serializer * lhs, const basic_serializer * rhs
    ) const ;
};

class BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) basic_serializer_map : public
    boost::noncopyable
{
    typedef std::set<const basic_serializer *, type_info_pointer_compare> map_type;
    map_type m_map;
    bool & m_deleted;
public:
    bool insert(const basic_serializer * bs);
    const basic_serializer * tfind(
        const boost::serialization::extended_type_info & type_
    ) const;
    void erase(basic_serializer * bs);
    basic_serializer_map(bool & deleted);
    ~basic_serializer_map();
private:
    // cw 8.3 requires this
    basic_serializer_map& operator=(basic_serializer_map const&);
};

} // namespace detail
} // namespace archive
} // namespace boost

#include <boost/archive/detail/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif // BOOST_TYPEINFO_EXTENDED_MAP_HPP
