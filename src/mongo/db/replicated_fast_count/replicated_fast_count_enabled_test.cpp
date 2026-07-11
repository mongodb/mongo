// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

// TODO(SERVER-119896): Delete this test.
namespace mongo {
namespace {

class IsReplicatedFastCountEnabledTest : public CatalogTestFixture {};

class IsReplicatedFastCountEnabledWithProviderTest : public CatalogTestFixture {
public:
    IsReplicatedFastCountEnabledWithProviderTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count::test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}
};

// A persistence provider that reports both shouldUseReplicatedFastCount() and
// mustUseContainerWrites() as true.
// TODO(SERVER-126250): The shared test helper currently only flips shouldUseReplicatedFastCount()
// so we override it here.
class ListCollectionsFastCountProvider
    : public replicated_fast_count::test_helpers::ReplicatedFastCountTestPersistenceProvider {
public:
    bool shouldUseReplicatedFastCount() const override {
        return true;
    }

    bool mustUseContainerWrites() const override {
        return true;
    }
};

class IsReplicatedFastCountListCollectionsWithProviderTest : public CatalogTestFixture {
public:
    IsReplicatedFastCountListCollectionsWithProviderTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<ListCollectionsFastCountProvider>())) {}
};

TEST_F(IsReplicatedFastCountEnabledTest, DisabledWhenFeatureFlagOff) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", false);
    EXPECT_FALSE(isReplicatedFastCountEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, EnabledWhenFeatureFlagOn) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
    EXPECT_TRUE(isReplicatedFastCountEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledWithProviderTest, EnabledWhenProviderReturnsTrue) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", false);
    EXPECT_TRUE(isReplicatedFastCountEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledWithProviderTest, EnabledWhenBothFlagAndProviderOn) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
    EXPECT_TRUE(isReplicatedFastCountEnabled(operationContext()));
}

TEST(ReplicatedFastCountEligibleNsTest, NormalCollectionsEligible) {
    const NamespaceString normalNss =
        NamespaceString::createNamespaceString_forTest("test", "coll1");
    EXPECT_TRUE(isReplicatedFastCountEligible(normalNss));
}

TEST(ReplicatedFastCountEligibleNsTest, InternalLocalCollectionsNotEligible) {
    const NamespaceString localNss =
        NamespaceString::createNamespaceString_forTest("local", "coll1");
    EXPECT_FALSE(isReplicatedFastCountEligible(localNss));
}

TEST(ReplicatedFastCountEligibleNsTest, InternalNonLocalCollectionsEligible) {
    const NamespaceString configNss =
        NamespaceString::createNamespaceString_forTest("config", "coll1");
    EXPECT_TRUE(isReplicatedFastCountEligible(configNss));

    const NamespaceString adminNss =
        NamespaceString::createNamespaceString_forTest("admin", "coll1");
    EXPECT_TRUE(isReplicatedFastCountEligible(adminNss));
}

TEST(ReplicatedFastCountEligibleNsTest, ImplicitlyReplicatedNotEligible) {
    const NamespaceString configTransactionsNss =
        NamespaceString::createNamespaceString_forTest("config", "transactions");
    EXPECT_FALSE(isReplicatedFastCountEligible(configTransactionsNss));

    const NamespaceString configPreimagesNss =
        NamespaceString::createNamespaceString_forTest("config", "system.preimages");
    EXPECT_FALSE(isReplicatedFastCountEligible(configPreimagesNss));

    const NamespaceString configImageCollectionNss =
        NamespaceString::createNamespaceString_forTest("config", "image_collection");
    EXPECT_FALSE(isReplicatedFastCountEligible(configImageCollectionNss));
}

TEST(ReplicatedFastCountEligibleNsTest, SizeCountAndTimestampStoresNotEligible) {
    const NamespaceString fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    EXPECT_FALSE(isReplicatedFastCountEligible(fastCountStoreNss));

    const NamespaceString fastCountTimestampStoreNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);
    EXPECT_FALSE(isReplicatedFastCountEligible(fastCountTimestampStoreNss));
}

TEST(ReplicatedFastCountEligibleNsTest, AdminSystemVersionNotEligible) {
    EXPECT_FALSE(isReplicatedFastCountEligible(NamespaceString::kServerConfigurationNamespace));
}

TEST(ReplicatedFastCountEligibleNsTest, SystemProfileNotEligible) {
    const NamespaceString systemProfileNss =
        NamespaceString::createNamespaceString_forTest("test", "system.profile");
    EXPECT_FALSE(isReplicatedFastCountEligible(systemProfileNss));
}

TEST(ReplicatedFastCountEligibleNsTest, OplogEligible) {
    EXPECT_TRUE(isReplicatedFastCountEligible(NamespaceString::kRsOplogNamespace));
}

TEST_F(IsReplicatedFastCountEnabledTest, ListCollectionsDisabledWhenBothFlagsOff) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", false);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", false);
    EXPECT_FALSE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, ListCollectionsDisabledWhenOnlyReplicatedFastCountOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", false);
    EXPECT_FALSE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, ListCollectionsDisabledWhenOnlyContainerWritesOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", false);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    EXPECT_FALSE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, ListCollectionsEnabledWhenBothFlagsOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    EXPECT_TRUE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, ListCollectionsDisabledWhenTestCommandsOff) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    setTestCommandsEnabled(false);
    const auto restoreTestCommands = ScopeGuard([] { setTestCommandsEnabled(true); });
    EXPECT_FALSE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

// Test that we only try to move initial sync's beginFetchingOptime when replicated fast count and
// test commands are enabled.
TEST_F(IsReplicatedFastCountEnabledTest, InitialSyncDisabledWhenReplicatedFastCountOff) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", false);
    EXPECT_FALSE(isReplicatedFastCountInitialSyncEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, InitialSyncEnabledWhenReplicatedFastCountOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", true);
    EXPECT_TRUE(isReplicatedFastCountInitialSyncEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, InitialSyncDisabledWhenTestCommandsOff) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", true);
    setTestCommandsEnabled(false);
    const auto restoreTestCommands = ScopeGuard([] { setTestCommandsEnabled(true); });
    EXPECT_FALSE(isReplicatedFastCountInitialSyncEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest,
       InitialSyncDisabledWhenReplicatedFastCountOffAndTestCommandsOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", false);
    setTestCommandsEnabled(false);
    const auto restoreTestCommands = ScopeGuard([] { setTestCommandsEnabled(true); });
    EXPECT_FALSE(isReplicatedFastCountInitialSyncEnabled(operationContext()));
}

// The following tests verify that listCollections fast count emission is gated solely on the
// feature flags and is NOT enabled by persistence provider traits.
TEST_F(IsReplicatedFastCountListCollectionsWithProviderTest,
       ListCollectionsDisabledWithProviderWhenBothFlagsOff) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", false);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", false);
    EXPECT_FALSE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountListCollectionsWithProviderTest,
       ListCollectionsDisabledWithProviderWhenOnlyReplicatedFastCountOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", false);
    EXPECT_FALSE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountListCollectionsWithProviderTest,
       ListCollectionsDisabledWithProviderWhenOnlyContainerWritesOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", false);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    EXPECT_FALSE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

// Sanity check: with the provider present and both flags on, emission is still enabled (the
// provider neither enables nor disables the feature on its own).
TEST_F(IsReplicatedFastCountListCollectionsWithProviderTest,
       ListCollectionsEnabledWithProviderWhenBothFlagsOn) {
    unittest::ServerParameterGuard ffReplicatedFastCount("featureFlagReplicatedFastCount", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    EXPECT_TRUE(isReplicatedFastCountListCollectionsEnabled(operationContext()));
}

// Tuple of dbName, collName, and whether shouldReadFromReplicatedFastCount is expected to return
// true for this test case.
using ShouldReadFromRFCTestCase = std::tuple<std::string, std::string, bool>;

struct ShouldReadFromReplicatedFastCountParams {
    std::string name;
    bool featureFlagOn;
    bool hasProvider;
    std::vector<ShouldReadFromRFCTestCase> testCases;
};

class ShouldReadFromReplicatedFastCountTestWithParams
    : public CatalogTestFixture,
      public ::testing::WithParamInterface<ShouldReadFromReplicatedFastCountParams> {
public:
    ShouldReadFromReplicatedFastCountTestWithParams()
        : CatalogTestFixture(
              GetParam().hasProvider
                  ? Options().setPersistenceProvider(
                        std::make_unique<replicated_fast_count::test_helpers::
                                             ReplicatedFastCountTestPersistenceProvider>())
                  : Options()) {}
};

TEST_P(ShouldReadFromReplicatedFastCountTestWithParams, ShouldReadFromReplicatedFastCount) {
    const auto& p = GetParam();

    unittest::ServerParameterGuard flag("featureFlagReplicatedFastCount", p.featureFlagOn);

    for (const auto& [dbName, collName, expected] : p.testCases) {
        const auto nss = NamespaceString::createNamespaceString_forTest(dbName, collName);
        EXPECT_EQ(shouldReadFromReplicatedFastCount(operationContext(), nss), expected)
            << "shouldReadFromReplicatedFastCount failed for " << nss.toStringForErrorMsg();
    }
}

INSTANTIATE_TEST_SUITE_P(
    ShouldReadFromReplicatedFastCount,
    ShouldReadFromReplicatedFastCountTestWithParams,
    ::testing::Values(
        ShouldReadFromReplicatedFastCountParams{
            "NoProviderFFOff",
            /*featureFlagOn=*/false,
            /*hasProvider=*/false,
            {
                // We do not read from the replicated fast count system if the provider does not use
                // it and if the feature flag is off.
                {"test", "coll1", false},
                {"config", "coll1", false},
                {"admin", "coll1", false},
                {"local", "coll1", false},
                {"config", "transactions", false},
                {"config", "system.preimages", false},
                {"config", "image_collection", false},
                {"config", "system.profile", false},
                {"config", std::string{NamespaceString::kReplicatedFastCountStore}, false},
                {"config",
                 std::string{NamespaceString::kReplicatedFastCountStoreTimestamps},
                 false},
                {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
                 std::string{NamespaceString::kRsOplogNamespace.coll()},
                 false},
            },
        },
        ShouldReadFromReplicatedFastCountParams{
            "NoProviderFFOn",
            /*featureFlagOn=*/true,
            /*hasProvider=*/false,
            {
                {"test", "coll1", true},
                {"config", "coll1", true},
                {"admin", "coll1", true},
                // If the persistence provider does not support replicated fast count, the oplog's
                // size and count should not be read from the replicated fast count system.
                {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
                 std::string{NamespaceString::kRsOplogNamespace.coll()},
                 false},
                // Collections with ineligible namespaces should not be read from the RFC system.
                {"local", "coll1", false},
                {"config", "transactions", false},
                {"config", "system.preimages", false},
                {"config", "image_collection", false},
                {"config", "system.profile", false},
                {"config", std::string{NamespaceString::kReplicatedFastCountStore}, false},
                {"config",
                 std::string{NamespaceString::kReplicatedFastCountStoreTimestamps},
                 false},
            },
        },
        ShouldReadFromReplicatedFastCountParams{
            "WithProviderFFOff",
            /*featureFlagOn=*/false,
            /*hasProvider=*/true,
            {
                {"test", "coll1", true},
                {"config", "coll1", true},
                {"admin", "coll1", true},
                {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
                 std::string{NamespaceString::kRsOplogNamespace.coll()},
                 true},
                // Collections with ineligible namespaces should not be read from the RFC system.
                {"local", "coll1", false},
                {"config", "transactions", false},
                {"config", "system.preimages", false},
                {"config", "image_collection", false},
                {"config", "system.profile", false},
                {"config", std::string{NamespaceString::kReplicatedFastCountStore}, false},
                {"config",
                 std::string{NamespaceString::kReplicatedFastCountStoreTimestamps},
                 false},
            },
        },
        ShouldReadFromReplicatedFastCountParams{
            "WithProviderFFOn",
            /*featureFlagOn=*/true,
            /*hasProvider=*/true,
            {
                // Results are the same as WithProviderFFOff.
                {"test", "coll1", true},
                {"config", "coll1", true},
                {"admin", "coll1", true},
                {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
                 std::string{NamespaceString::kRsOplogNamespace.coll()},
                 true},
                {"local", "coll1", false},
                {"config", "transactions", false},
                {"config", "system.preimages", false},
                {"config", "image_collection", false},
                {"config", "system.profile", false},
                {"config", std::string{NamespaceString::kReplicatedFastCountStore}, false},
                {"config",
                 std::string{NamespaceString::kReplicatedFastCountStoreTimestamps},
                 false},
            },
        }),
    [](const ::testing::TestParamInfo<ShouldReadFromReplicatedFastCountParams>& info) {
        return info.param.name;
    });

class ShouldReadFromReplicatedFastCountOfflineValidateTest : public CatalogTestFixture {
public:
    ShouldReadFromReplicatedFastCountOfflineValidateTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count::test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}

    void setUp() override {
        CatalogTestFixture::setUp();
        // Mock the fixture being a standalone by resetting its repl settings.
        repl::ReplicationCoordinator::set(getServiceContext(),
                                          std::make_unique<repl::ReplicationCoordinatorMock>(
                                              getServiceContext(), repl::ReplSettings{}));
    }
};

TEST_F(ShouldReadFromReplicatedFastCountOfflineValidateTest, ReturnsTrue) {
    // Set the storageGlobalParams.validate flag to true to simulate modal validation.
    storageGlobalParams.validate = true;
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll1");
    EXPECT_TRUE(shouldReadFromReplicatedFastCount(operationContext(), nss));
    // Reset state.
    storageGlobalParams.validate = false;
}

}  // namespace
}  // namespace mongo
