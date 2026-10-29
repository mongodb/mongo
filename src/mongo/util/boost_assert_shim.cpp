// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/boost_assert_shim.h"

#if defined(BOOST_ENABLE_ASSERT_DEBUG_HANDLER) && !defined(NDEBUG)

#include <exception>

#include <boost/assert.hpp>

namespace mongo {
BoostAssertFuncs& BoostAssertFuncs::global() {
    static BoostAssertFuncs funcs;
    return funcs;
}
}  // namespace mongo

namespace boost {
void assertion_failed(char const* expr, char const* function, char const* file, long line) {
    auto assertFunc = ::mongo::BoostAssertFuncs::global().assertFunc;
    if (!assertFunc)
        std::terminate();
    assertFunc(expr, function, file, line);
}

void assertion_failed_msg(
    char const* expr, char const* msg, char const* function, char const* file, long line) {
    auto assertMsgFunc = ::mongo::BoostAssertFuncs::global().assertMsgFunc;
    if (!assertMsgFunc)
        std::terminate();
    assertMsgFunc(expr, msg, function, file, line);
}

}  // namespace boost
#endif  // BOOST_ENABLE_ASSERT_DEBUG_HANDLER && !NDEBUG
