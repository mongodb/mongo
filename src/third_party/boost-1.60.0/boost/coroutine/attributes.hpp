
//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_COROUTINES_ATTRIBUTES_H
#define BOOST_COROUTINES_ATTRIBUTES_H

#include <cstddef>

#include <boost/config.hpp>

#include <boost/coroutine/flags.hpp>
#include <boost/coroutine/stack_allocator.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace boost {
namespace coroutines {

struct attributes
{
    std::size_t     size;
    flag_unwind_t   do_unwind;
    flag_fpu_t      preserve_fpu;

    attributes() BOOST_NOEXCEPT :
        size( stack_allocator::traits_type::default_size() ),
        do_unwind( stack_unwind),
        preserve_fpu( fpu_preserved)
    {}

    explicit attributes( std::size_t size_) BOOST_NOEXCEPT :
        size( size_),
        do_unwind( stack_unwind),
        preserve_fpu( fpu_preserved)
    {}

    explicit attributes( flag_unwind_t do_unwind_) BOOST_NOEXCEPT :
        size( stack_allocator::traits_type::default_size() ),
        do_unwind( do_unwind_),
        preserve_fpu( fpu_preserved)
    {}

    explicit attributes( flag_fpu_t preserve_fpu_) BOOST_NOEXCEPT :
        size( stack_allocator::traits_type::default_size() ),
        do_unwind( stack_unwind),
        preserve_fpu( preserve_fpu_)
    {}

    explicit attributes(
            std::size_t size_,
            flag_unwind_t do_unwind_) BOOST_NOEXCEPT :
        size( size_),
        do_unwind( do_unwind_),
        preserve_fpu( fpu_preserved)
    {}

    explicit attributes(
            std::size_t size_,
            flag_fpu_t preserve_fpu_) BOOST_NOEXCEPT :
        size( size_),
        do_unwind( stack_unwind),
        preserve_fpu( preserve_fpu_)
    {}

    explicit attributes(
            flag_unwind_t do_unwind_,
            flag_fpu_t preserve_fpu_) BOOST_NOEXCEPT :
        size( stack_allocator::traits_type::default_size() ),
        do_unwind( do_unwind_),
        preserve_fpu( preserve_fpu_)
    {}

    explicit attributes(
            std::size_t size_,
            flag_unwind_t do_unwind_,
            flag_fpu_t preserve_fpu_) BOOST_NOEXCEPT :
        size( size_),
        do_unwind( do_unwind_),
        preserve_fpu( preserve_fpu_)
    {}
};

}}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

#endif // BOOST_COROUTINES_ATTRIBUTES_H
