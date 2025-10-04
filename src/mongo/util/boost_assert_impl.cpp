/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
        invariantFailed(expr, makeLoc(function, file, line));
    };
    funcs.assertMsgFunc =
        [](char const* expr, char const* msg, char const* function, char const* file, long line) {
            invariantFailedWithMsg(expr, msg, makeLoc(function, file, line));
        };
    return 0;
}();
}  // namespace
}  // namespace mongo

#endif  // BOOST_ENABLE_ASSERT_DEBUG_HANDLER && !NDEBUG
