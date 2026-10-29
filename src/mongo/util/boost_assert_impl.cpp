// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/source_location.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/boost_assert_shim.h"

#include <functional>
#include <memory>

#if defined(BOOST_ENABLE_ASSERT_DEBUG_HANDLER) && !defined(NDEBUG)

namespace mongo {
namespace {
SyntheticSourceLocation makeLoc(char const* function, char const* file, long line) {
    return SyntheticSourceLocation{file, static_cast<uint_least32_t>(line), function};
}
auto installBoostAssertCallbacks = [] {
    auto&& funcs = BoostAssertFuncs::global();
    funcs.assertFunc = [](char const* expr, char const* function, char const* file, long line) {
        error_details::invariantFailed(expr, makeLoc(function, file, line));
    };
    funcs.assertMsgFunc =
        [](char const* expr, char const* msg, char const* function, char const* file, long line) {
            error_details::invariantFailedWithMsg(expr, msg, makeLoc(function, file, line));
        };
    return 0;
}();
}  // namespace
}  // namespace mongo

#endif  // BOOST_ENABLE_ASSERT_DEBUG_HANDLER && !NDEBUG
