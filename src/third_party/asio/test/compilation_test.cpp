// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/unittest.h"

#include <asio/io_context.hpp>
#include <asio/post.hpp>

namespace mongo {
namespace {

/**
 * Verify that we can compile a program that includes asio headers and that links to asio code.
 */
TEST(AsioCompilationTest, CanCompile) {
    asio::io_context io_ctx;
    bool ran = false;
    asio::post(io_ctx, [&]() { ran = true; });
    io_ctx.run();
    ASSERT(ran);
}

}  // namespace
}  // namespace mongo
