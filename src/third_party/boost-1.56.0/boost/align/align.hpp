/*
 Copyright (c) 2014 Glen Joseph Fernandes
 glenfe at live dot com

 Distributed under the Boost Software License,
 Version 1.0. (See accompanying file LICENSE_1_0.txt
 or copy at http://boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_ALIGN_ALIGN_HPP
#define BOOST_ALIGN_ALIGN_HPP

/**
 Function align.

 @file
 @author Glen Fernandes
*/

#include <boost/config.hpp>

/**
 @cond
*/
#if !defined(BOOST_NO_CXX11_STD_ALIGN)
#include <boost/align/detail/align_cxx11.hpp>
#else
#include <boost/align/detail/align.hpp>
#endif

#if defined(BOOST_NO_CXX11_STD_ALIGN)
/**
 @endcond
*/

/**
 Boost namespace.
*/
namespace boost {
    /**
     Alignment namespace.
    */
    namespace alignment {
        /**
         If it is possible to fit `size` bytes of storage
         aligned by `alignment` into the buffer pointed to by
         `ptr` with length `space`, the function updates `ptr`
         to point to the first possible address of such
         storage and decreases `space` by the number of bytes
         used for alignment. Otherwise, the function does
         nothing.

         @param alignment Shall be a fundamental alignment
           value or an extended alignment value, and shall be
           a power of two.

         @param size The size in bytes of storage to fit into
           the buffer.

         @param ptr Shall point to contiguous storage of at
           least `space` bytes.

         @param space The length of the buffer.

         @return A null pointer if the requested aligned
           buffer would not fit into the available space,
           otherwise the adjusted value of `ptr`.

         @remark **Note:** The function updates its `ptr` and
           space arguments so that it can be called repeatedly
           with possibly different `alignment` and `size`
           arguments for the same buffer.
        */
        inline void* align(std::size_t alignment, std::size_t size,
            void*& ptr, std::size_t& space);
    }
}

/**
 @cond
*/
#endif
/**
 @endcond
*/

#endif
