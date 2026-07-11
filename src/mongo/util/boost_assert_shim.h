// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#if defined(BOOST_ENABLE_ASSERT_DEBUG_HANDLER) && !defined(NDEBUG)
#include "mongo/util/modules.h"

#include <functional>

namespace mongo {
struct BoostAssertFuncs {
    std::function<void(char const* expr, char const* function, char const* file, long line)>
        assertFunc;
    std::function<void(
        char const* expr, char const* msg, char const* function, char const* file, long line)>
        assertMsgFunc;

    static BoostAssertFuncs& global();

private:
    BoostAssertFuncs() = default;
};

}  // namespace mongo

#endif  // BOOST_ENABLE_ASSERT_DEBUG_HANDLER && !NDEBUG
