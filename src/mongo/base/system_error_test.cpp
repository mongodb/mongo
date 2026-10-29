// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/system_error.h"

#include "mongo/unittest/unittest.h"

#include <memory>
#include <system_error>

namespace mongo {
namespace {

TEST(SystemError, Category) {
    ASSERT(make_error_code(ErrorCodes::AuthenticationFailed).category() == mongoErrorCategory());
    ASSERT(std::error_code(ErrorCodes::AlreadyInitialized, mongoErrorCategory()).category() ==
           mongoErrorCategory());
    ASSERT(make_error_condition(ErrorCodes::AuthenticationFailed).category() ==
           mongoErrorCategory());
    ASSERT(std::error_condition(ErrorCodes::AuthenticationFailed).category() ==
           mongoErrorCategory());
}

TEST(SystemError, Conversions) {
    ASSERT(make_error_code(ErrorCodes::AlreadyInitialized) == ErrorCodes::AlreadyInitialized);
    ASSERT(std::error_code(ErrorCodes::AlreadyInitialized, mongoErrorCategory()) ==
           ErrorCodes::AlreadyInitialized);
    ASSERT(make_error_condition(ErrorCodes::AlreadyInitialized) == ErrorCodes::AlreadyInitialized);
    ASSERT(std::error_condition(ErrorCodes::AlreadyInitialized) == ErrorCodes::AlreadyInitialized);
}

TEST(SystemError, Equivalence) {
    ASSERT(ErrorCodes::OK == std::error_code());
    ASSERT(std::error_code() == ErrorCodes::OK);
}

}  // namespace
}  // namespace mongo
