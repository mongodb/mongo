/*
(c) 2014-2015 Glen Joseph Fernandes
<glenjofe -at- gmail.com>

Distributed under the Boost Software
License, Version 1.0.
http://boost.org/LICENSE_1_0.txt
*/
#ifndef BOOST_ALIGN_DETAIL_ALIGNED_ALLOC_HPP
#define BOOST_ALIGN_DETAIL_ALIGNED_ALLOC_HPP

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/align/align.hpp>
#include <boost/align/alignment_of.hpp>
#include <boost/align/detail/is_alignment.hpp>
#include <cstdlib>

namespace boost {
namespace alignment {

inline void* aligned_alloc(std::size_t alignment, std::size_t size)
    BOOST_NOEXCEPT
{
    BOOST_ASSERT(detail::is_alignment(alignment));
    enum {
        N = alignment_of<void*>::value
    };
    if (alignment < N) {
        alignment = N;
    }
    std::size_t n = size + alignment - N;
    void* p1 = 0;
    void* p2 = std::malloc(n + sizeof p1);
    if (p2) {
        p1 = static_cast<char*>(p2) + sizeof p1;
        (void)align(alignment, size, p1, n);
        *(static_cast<void**>(p1) - 1) = p2;
    }
    return p1;
}

inline void aligned_free(void* ptr) BOOST_NOEXCEPT
{
    if (ptr) {
        void* p = *(static_cast<void**>(ptr) - 1);
        std::free(p);
    }
}

} /* .alignment */
} /* .boost */

#endif
