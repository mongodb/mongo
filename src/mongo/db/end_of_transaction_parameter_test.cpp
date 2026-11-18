// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/end_of_transaction_parameter.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/end_of_transaction_parameter_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"
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

// Reads the allowEndOfTransaction gauge from the serverStatus metric tree rendered for
// ClusterRole::None. Appending the metric tree requires a global ServiceContext: metrics
// registered by transitively linked libraries read the service context's clock while serializing.
// Creating the ServiceContext also fires the ConstructorActionRegisterer that seeds the gauge with
// the parameter's current value (mirroring what the servers do at startup).
long long readAllowEndOfTransactionGauge() {
    if (!hasGlobalServiceContext()) {
        setGlobalServiceContext(ServiceContext::make());
    }

    BSONObjBuilder bob;
    globalMetricTreeSet()[ClusterRole::None].appendTo(bob);
    BSONObj root = bob.obj();  // Must outlive obj: embeddedObject() is a non-owning view.
    auto metricsEl = root["metrics"];
    if (metricsEl.eoo())
        return 0LL;
    auto changeStreamsEl = metricsEl.embeddedObject()["changeStreams"];
    if (changeStreamsEl.eoo())
        return 0LL;
    auto eotEl = changeStreamsEl.embeddedObject()["endOfTransaction"];
    if (eotEl.eoo())
        return 0LL;
    auto gaugeEl = eotEl.embeddedObject()["allowEndOfTransaction"];
    return gaugeEl.eoo() ? 0LL : gaugeEl.numberLong();
}

TEST(AllowEndOfTransactionParameterTest, RestoresDefaultAfterGuardDestroyed) {
    {
        unittest::ServerParameterGuard guard("allowEndOfTransaction", false);
        ASSERT_FALSE(isAllowEndOfTransactionEnabled());
    }
    ASSERT_TRUE(isAllowEndOfTransactionEnabled());
}

TEST(AllowEndOfTransactionParameterTest, GaugeMirrorsParameterValue) {
    // The parameter defaults to enabled, and the gauge is seeded at registration.
    ASSERT_EQ(readAllowEndOfTransactionGauge(), 1);

    {
        unittest::ServerParameterGuard guard("allowEndOfTransaction", false);
        ASSERT_EQ(readAllowEndOfTransactionGauge(), 0);
    }
    ASSERT_EQ(readAllowEndOfTransactionGauge(), 1);
}

// Directly exercises the on_update callback: the gauge reflects the value passed to the callback,
// exactly as it does for a real setParameter-driven update.
TEST(AllowEndOfTransactionParameterTest, OnUpdateCallbackSetsGaugeFromArgument) {
    // The parameter defaults to enabled, and the gauge is seeded at registration.
    ASSERT_EQ(readAllowEndOfTransactionGauge(), 1);

    ASSERT_OK(onUpdateAllowEndOfTransaction(false));
    ASSERT_EQ(readAllowEndOfTransactionGauge(), 0);

    ASSERT_OK(onUpdateAllowEndOfTransaction(true));
    ASSERT_EQ(readAllowEndOfTransactionGauge(), 1);
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
