
//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "boost/coroutine/detail/coroutine_context.hpp"

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable:4355)
#endif

#if defined(BOOST_USE_SEGMENTED_STACKS)
extern "C" {

void __splitstack_getcontext( void * [BOOST_COROUTINES_SEGMENTS]);

void __splitstack_setcontext( void * [BOOST_COROUTINES_SEGMENTS]);

}
#endif

namespace boost {
namespace coroutines {
namespace detail {

coroutine_context::coroutine_context() :
    palloc_(),
    ctx_( 0)
{}

coroutine_context::coroutine_context( ctx_fn fn, preallocated const& palloc) :
    palloc_( palloc),
    ctx_( context::make_fcontext( palloc_.sp, palloc_.size, fn) )
{}

coroutine_context::coroutine_context( coroutine_context const& other) :
    palloc_( other.palloc_),
    ctx_( other.ctx_)
{}

coroutine_context &
coroutine_context::operator=( coroutine_context const& other)
{
    if ( this == & other) return * this;

    palloc_ = other.palloc_;
    ctx_ = other.ctx_;

    return * this;
}

intptr_t
coroutine_context::jump( coroutine_context & other, intptr_t param, bool preserve_fpu)
{
#if defined(BOOST_USE_SEGMENTED_STACKS)
    __splitstack_getcontext( palloc_.sctx.segments_ctx);
    __splitstack_setcontext( other.palloc_.sctx.segments_ctx);

    intptr_t ret = context::jump_fcontext( & ctx_, other.ctx_, param, preserve_fpu);

    __splitstack_setcontext( palloc_.sctx.segments_ctx);

    return ret;
#else
    return context::jump_fcontext( & ctx_, other.ctx_, param, preserve_fpu);
#endif
}

}}}

#if defined(_MSC_VER)
# pragma warning(pop)
#endif

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif
