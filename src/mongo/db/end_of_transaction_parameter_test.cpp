// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/end_of_transaction_parameter.h"

#include "mongo/db/end_of_transaction_parameter_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(AllowEndOfTransactionParameterTest, DefaultsToEnabled) {
    ASSERT_TRUE(gAllowEndOfTransaction.load());
    ASSERT_TRUE(isAllowEndOfTransactionEnabled());
}

TEST(AllowEndOfTransactionParameterTest, CanBeDisabledAtRuntime) {
    unittest::ServerParameterGuard guard("allowEndOfTransaction", false);
    ASSERT_FALSE(isAllowEndOfTransactionEnabled());
}

TEST(AllowEndOfTransactionParameterTest, RestoresDefaultAfterGuardDestroyed) {
    {
        unittest::ServerParameterGuard guard("allowEndOfTransaction", false);
        ASSERT_FALSE(isAllowEndOfTransactionEnabled());
    }
    ASSERT_TRUE(isAllowEndOfTransactionEnabled());
}

}  // namespace
}  // namespace mongo
