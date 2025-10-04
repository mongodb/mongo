
//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//        http://www.boost.org/LICENSE_1_0.txt)

#include <boost/context/detail/fcontext.hpp>

using boost::context::detail::fcontext_t;
using boost::context::detail::transfer_t;

// This C++ tail of ontop_fcontext() allocates transfer_t{ from, vp }
// on the stack.  If fn() throws a C++ exception, then the C++ runtime
// must remove this tail's stack frame.
extern "C" transfer_t
ontop_fcontext_tail( int ignore, void * vp, transfer_t (* fn)(transfer_t), fcontext_t const from) {
    return fn( transfer_t{ from, vp });
}
