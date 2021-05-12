
//          Copyright Oliver Kowalke 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#if defined(BOOST_USE_UCONTEXT)
#include "boost/context/continuation_ucontext.hpp"
#elif defined(BOOST_USE_WINFIB)
#include "boost/context/continuation_winfib.hpp"
#else
#include "boost/context/execution_context.hpp"
#endif

#include <boost/config.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_PREFIX
#endif

namespace boost {
namespace context {
namespace detail {

// zero-initialization
thread_local activation_record * current_rec;
thread_local static std::size_t counter;

// schwarz counter
activation_record_initializer::activation_record_initializer() noexcept {
    if ( 0 == counter++) {
        current_rec = new activation_record();
    }
}

activation_record_initializer::~activation_record_initializer() {
    if ( 0 == --counter) {
        BOOST_ASSERT( current_rec->is_main_context() );
        delete current_rec;
    }
}

}

namespace detail {

activation_record *&
activation_record::current() noexcept {
    // initialized the first time control passes; per thread
    thread_local static activation_record_initializer initializer;
    return current_rec;
}

}

}}

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_SUFFIX
#endif
