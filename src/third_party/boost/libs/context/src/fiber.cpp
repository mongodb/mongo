
//          Copyright Oliver Kowalke 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#if defined(BOOST_USE_UCONTEXT)
#include "boost/context/fiber_ucontext.hpp"
#elif defined(BOOST_USE_WINFIB)
#include "boost/context/fiber_winfib.hpp"
#endif

#include <boost/config.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_PREFIX
#endif

namespace boost {
namespace context {
namespace detail {

// zero-initialization
thread_local fiber_activation_record * fib_current_rec;
thread_local static std::size_t counter;

// schwarz counter
fiber_activation_record_initializer::fiber_activation_record_initializer() noexcept {
    if ( 0 == counter++) {
        fib_current_rec = new fiber_activation_record();
    }
}

fiber_activation_record_initializer::~fiber_activation_record_initializer() {
    if ( 0 == --counter) {
        BOOST_ASSERT( fib_current_rec->is_main_context() );
        delete fib_current_rec;
    }
}

}

namespace detail {

fiber_activation_record *&
fiber_activation_record::current() noexcept {
    // initialized the first time control passes; per thread
    thread_local static fiber_activation_record_initializer initializer;
    return fib_current_rec;
}

}

}}

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_SUFFIX
#endif
