/*
(c) 2014 Glen Joseph Fernandes
<glenjofe -at- gmail.com>

Distributed under the Boost Software
License, Version 1.0.
http://boost.org/LICENSE_1_0.txt
*/
#ifndef BOOST_ALIGN_DETAIL_ALIGN_HPP
#define BOOST_ALIGN_DETAIL_ALIGN_HPP

#include <boost/assert.hpp>
#include <boost/align/detail/address.hpp>
#include <boost/align/detail/is_alignment.hpp>
#include <cstddef>

namespace boost {
namespace alignment {

inline void* align(std::size_t alignment, std::size_t size,
    void*& ptr, std::size_t& space)
{
    BOOST_ASSERT(detail::is_alignment(alignment));
    std::size_t n = detail::address(ptr) & (alignment - 1);
    if (n != 0) {
        n = alignment - n;
    }
    void* p = 0;
    if (n <= space && size <= space - n) {
        p = static_cast<char*>(ptr) + n;
        ptr = p;
        space -= n;
    }
    return p;
}

} /* .alignment */
} /* .boost */

#endif
