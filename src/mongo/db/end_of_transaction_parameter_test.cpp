// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/end_of_transaction_parameter.h"

#include "mongo/db/end_of_transaction_parameter_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"

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

TEST(EndOfTransactionWritePathTest, EnabledOnlyWhenFlagAndParameterAreEnabled) {
    unittest::ServerParameterGuard flag("featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard parameter("allowEndOfTransaction", true);
    ASSERT_TRUE(isEndOfTransactionWritePathEnabled());
}

TEST(EndOfTransactionWritePathTest, DisabledWhenParameterDisabled) {
    unittest::ServerParameterGuard flag("featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard parameter("allowEndOfTransaction", false);
    ASSERT_FALSE(isEndOfTransactionWritePathEnabled());
}

TEST(EndOfTransactionWritePathTest, DisabledWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard flag("featureFlagEndOfTransactionChangeEventSynthesis", false);
    unittest::ServerParameterGuard parameter("allowEndOfTransaction", true);
    ASSERT_FALSE(isEndOfTransactionWritePathEnabled());
}

TEST(EndOfTransactionWritePathTest, EnabledRegardlessOfFCV) {
    unittest::ServerParameterGuard flag("featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard parameter("allowEndOfTransaction", true);

    // The write path is not FCV gated: it is safe to attach the metadata even when the cluster FCV
    // is below latest, because a shard whose binary predates the field rejects the commit command
    // and the coordinator retries without the metadata.
    // (Generic FCV reference): this test asserts the same behavior at both the last-LTS and the
    // latest FCV.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
    ASSERT_TRUE(isEndOfTransactionWritePathEnabled());

    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
    ASSERT_TRUE(isEndOfTransactionWritePathEnabled());
}

}  // namespace
}  // namespace mongo
