// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_state.h"

#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::collection_validation {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.validateState");
const ValidationOptions kValidationOptions(ValidateMode::kForeground,
                                           RepairMode::kNone,
                                           /*logDiagnostics=*/false);
const ValidationOptions kValidationOptionsEnforceFastCount(
    ValidateMode::kForegroundFullEnforceFastCount,
    RepairMode::kNone,
    /*logDiagnostics=*/false);

class ValidateStateTest : public CatalogTestFixture {};

class ValidateStateWithoutSizeStorerTest : public CatalogTestFixture {
public:
    ValidateStateWithoutSizeStorerTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count::test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}
};

}  // namespace

TEST_F(ValidateStateTest, GetDetectedFastCountTypeReturnsLegacySizeStorer) {
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()),
              FastCountType::legacySizeStorer);
};

TEST_F(ValidateStateTest, GetDetectedFastCountTypeReturnsBoth) {
    ASSERT_OK(replicated_fast_count::createReplicatedFastCountCollection(storageInterface(),
                                                                         operationContext()));
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()), FastCountType::both);
};

TEST_F(ValidateStateWithoutSizeStorerTest, GetDetectedFastCountTypeReturnsReplicated) {
    ASSERT_OK(replicated_fast_count::createReplicatedFastCountCollection(storageInterface(),
                                                                         operationContext()));
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()),
              FastCountType::replicated);
}

TEST_F(ValidateStateWithoutSizeStorerTest, GetDetectedFastCountTypeReturnsNeither) {
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()), FastCountType::neither);
}

TEST(FastCountTypeToStringTest, Works) {
    EXPECT_EQ(toString(FastCountType::legacySizeStorer), "legacySizeStorer");
    EXPECT_EQ(toString(FastCountType::replicated), "replicated");
    EXPECT_EQ(toString(FastCountType::both), "both");
    EXPECT_EQ(toString(FastCountType::neither), "neither");
}

TEST_F(ValidateStateTest, GetExpectedFastCountTypeReturnsLegacySizeStorer) {
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getExpectedFastCountType(operationContext()),
              FastCountType::legacySizeStorer);
};

TEST_F(ValidateStateTest, GetExpectedFastCountTypeReturnsLegacySizeStorerNoOplog) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
    // Reset the oplog collection, to simulate this node never having been part of a replica set.
    LocalOplogInfo::get(operationContext())->resetRecordStore();
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getExpectedFastCountType(operationContext()),
              FastCountType::legacySizeStorer);
};

TEST_F(ValidateStateTest, GetExpectedFastCountTypeReturnsBoth) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getExpectedFastCountType(operationContext()), FastCountType::both);
};

TEST_F(ValidateStateWithoutSizeStorerTest, GetExpectedFastCountTypeReturnsReplicated) {
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getExpectedFastCountType(operationContext()),
              FastCountType::replicated);
}

// Alias for test cases checking whether certain collections should have their fast count and size
// enforced under different situations. The tuple consists of (dbName, collName,
// enforceCountAndSize, expectedResult), where enforceCountAndSize represents whether the
// enforceFastCount and enforceFastSize parameters were passed in to the validate command with a
// value of true or false.
using ShouldEnforceFastSizeAndCountTestCase =
    std::tuple<std::string_view, std::string_view, bool, bool>;

auto runShouldEnforceTestCases =
    [](OperationContext* opCtx,
       const std::vector<ShouldEnforceFastSizeAndCountTestCase>& testCases,
       FastCountType type) {
        for (const auto& [dbName, collName, enforce, expected] : testCases) {
            const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                std::string{dbName} + "." + std::string{collName});
            const ValidationOptions options(
                enforce ? ValidateMode::kForegroundFullEnforceFastCountAndSize
                        : ValidateMode::kForeground,
                RepairMode::kNone,
                /*logDiagnostics=*/false);
            ValidateState validateState(opCtx, nss, options);
            EXPECT_EQ(validateState.shouldEnforceFastCount(opCtx, type), expected)
                << "shouldEnforceFastCount failed for " << nss.toStringForErrorMsg();
            EXPECT_EQ(validateState.shouldEnforceFastSize(opCtx, type), expected)
                << "shouldEnforceFastSize failed for " << nss.toStringForErrorMsg();
        }
    };

struct ShouldEnforceFastCountAndSizeParams {
    std::string name;
    bool featureFlagOn;        // Whether featureFlagReplicatedFastCount is on.
    bool createRfcCollection;  // Whether we should create a replicated fast count store.
    bool hasSizeStorer;
    FastCountType expectedFastCountType;
    std::vector<ShouldEnforceFastSizeAndCountTestCase> testCases;
};

class ShouldEnforceFastCountAndSizeTest
    : public CatalogTestFixture,
      public ::testing::WithParamInterface<ShouldEnforceFastCountAndSizeParams> {
public:
    ShouldEnforceFastCountAndSizeTest()
        : CatalogTestFixture(
              GetParam().hasSizeStorer
                  ? Options()
                  : Options().setPersistenceProvider(
                        std::make_unique<replicated_fast_count::test_helpers::
                                             ReplicatedFastCountTestPersistenceProvider>())) {}
};

TEST_P(ShouldEnforceFastCountAndSizeTest, ShouldEnforceFastCountAndSize) {
    const auto& p = GetParam();

    std::unique_ptr<unittest::ServerParameterGuard> flag;
    if (p.featureFlagOn) {
        flag = std::make_unique<unittest::ServerParameterGuard>("featureFlagReplicatedFastCount",
                                                                true);
    }

    if (p.createRfcCollection) {
        ASSERT_OK(replicated_fast_count::createReplicatedFastCountCollection(storageInterface(),
                                                                             operationContext()));
    }

    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    FastCountType fastCountType = validateState.getDetectedFastCountType(operationContext());
    EXPECT_EQ(fastCountType, p.expectedFastCountType);

    runShouldEnforceTestCases(operationContext(), p.testCases, fastCountType);
}

INSTANTIATE_TEST_SUITE_P(
    ShouldEnforceFastCountAndSize,
    ShouldEnforceFastCountAndSizeTest,
    ::testing::Values(
        ShouldEnforceFastCountAndSizeParams{
            "FeatureFlagOffProviderOff",
            /*featureFlagOn=*/false,
            /*createRfcCollection=*/false,
            /*hasSizeStorer=*/true,
            FastCountType::legacySizeStorer,
            {
                // User collections will have their fast size/count validated iff the
                // enforceFastCount/Size parameters are passed in as true.
                {"test", "coll", false, false},
                {"test", "coll", true, true},
                // The oplog is always ineligible because entries can be added to it during
                // validation.
                {"local", "oplog.rs", false, false},
                {"local", "oplog.rs", true, false},
                // Change stream pre-images are always ineligible.
                {"config", "system.preimages", true, false},
                {"config", "system.preimages", false, false},
                // Special config collections are always ineligible.
                {"config", "transactions", true, false},
                {"config", "transactions", false, false},
                {"config", "image_collection", true, false},
                {"config", "image_collection", false, false},
                // The indexBuilds collection is ineligible when not using replicated fast count.
                {"config", "system.indexBuilds", true, false},
                {"config", "system.indexBuilds", false, false},
                // Local collections will have their fast size/count validated iff the
                // enforceFastCount/Size parameters are passed in as true.
                {"admin", "test", true, true},
                {"config", "test", true, true},
                {"local", "test", false, false},
                {"local", "test", true, true},
            },
        },
        ShouldEnforceFastCountAndSizeParams{
            "FeatureFlagOnProviderOff",
            /*featureFlagOn=*/true,
            /*createRfcCollection=*/false,
            /*hasSizeStorer=*/true,
            FastCountType::legacySizeStorer,
            {
                // User collections will have their fast size/count validated iff the
                // enforceFastCount/Size parameters are passed in as true.
                {"test", "coll", false, false},
                {"test", "coll", true, true},
                // The oplog is always ineligible because entries can be added to it during
                // validation.
                {"local", "oplog.rs", true, false},
                {"local", "oplog.rs", false, false},
                // Change stream pre-images are always ineligible.
                {"config", "system.preimages", true, false},
                {"config", "system.preimages", false, false},
                // Special config collections are always ineligible.
                {"config", "transactions", true, false},
                {"config", "transactions", false, false},
                {"config", "image_collection", true, false},
                {"config", "image_collection", false, false},
                // The indexBuilds collection is ineligible when not using replicated fast count.
                {"config", "system.indexBuilds", true, false},
                {"config", "system.indexBuilds", false, false},
                // Config/Admin/Local collections will have their fast size/count validated iff the
                // enforceFastCount/Size parameters are passed in as true.
                {"admin", "test", true, true},
                {"admin", "test", false, false},
                {"config", "test", true, true},
                {"config", "test", false, false},
                {"local", "test", true, true},
                {"local", "test", false, false},
            },
        },
        ShouldEnforceFastCountAndSizeParams{
            "FeatureFlagOffProviderOn",
            /*featureFlagOn=*/false,
            /*createRfcCollection=*/true,
            /*hasSizeStorer=*/false,
            FastCountType::replicated,
            {
                // User collections will always have their fast size/count validated.
                {"test", "coll", true, true},
                {"test", "coll", false, true},
                // The oplog is always ineligible because entries can be added to it during
                // validation.
                {"local", "oplog.rs", true, false},
                {"local", "oplog.rs", false, false},
                // Change stream pre-images are always ineligible.
                {"config", "system.preimages", true, false},
                {"config", "system.preimages", false, false},
                // Special config collections are always ineligible.
                {"config", "transactions", true, false},
                {"config", "transactions", false, false},
                {"config", "image_collection", true, false},
                {"config", "image_collection", false, false},
                // The indexBuilds collection is always eligible when using replicated fast count.
                {"config", "system.indexBuilds", true, true},
                {"config", "system.indexBuilds", false, true},
                // Config/Admin collections will have their fast size/count validated iff the
                // enforceFastCount/Size parameters are passed in as true.
                {"admin", "test", true, true},
                {"config", "test", true, true},
                // Local collections are not tracked by the replicated fast count system and will
                // never have their fast size/count validated.
                {"local", "test", true, false},
                {"local", "test", false, false},
            },
        },
        ShouldEnforceFastCountAndSizeParams{
            "FeatureFlagOnProviderOn",
            /*featureFlagOn=*/true,
            /*createRfcCollection=*/true,
            /*hasSizeStorer=*/false,
            FastCountType::replicated,
            {
                // User collections will always have their fast size/count validated.
                {"test", "coll", true, true},
                {"test", "coll", false, true},
                // The oplog is always ineligible because entries can be added to it during
                // validation.
                {"local", "oplog.rs", true, false},
                {"local", "oplog.rs", false, false},
                // Change stream pre-images are always ineligible.
                {"config", "system.preimages", true, false},
                {"config", "system.preimages", false, false},
                // Special config collections are always ineligible.
                {"config", "transactions", true, false},
                {"config", "transactions", false, false},
                {"config", "image_collection", true, false},
                {"config", "image_collection", false, false},
                // The indexBuilds collection is always eligible when using replicated fast count.
                {"config", "system.indexBuilds", true, true},
                {"config", "system.indexBuilds", false, true},
                // Config/Admin collections will have their fast size/count validated iff the
                // enforceFastCount/Size parameters are passed in as true.
                {"admin", "test", true, true},
                {"config", "test", true, true},
                // Local collections are not tracked by the replicated fast count system and will
                // never have their fast size/count validated.
                {"local", "test", true, false},
                {"local", "test", false, false},
            },
        },
        ShouldEnforceFastCountAndSizeParams{
            "BothStoresPresent",
            /*featureFlagOn=*/false,
            /*createRfcCollection=*/true,
            /*hasSizeStorer=*/true,
            FastCountType::both,
            {
                {"test", "coll", true, false},
                {"test", "coll", false, false},
                {"local", "oplog.rs", true, false},
                {"local", "oplog.rs", false, false},
                {"config", "system.preimages", true, false},
                {"config", "system.preimages", false, false},
                {"config", "system.indexBuilds", true, false},
                {"config", "system.indexBuilds", false, false},
                {"config", "transactions", true, false},
                {"config", "transactions", false, false},
                {"config", "image_collection", true, false},
                {"config", "image_collection", false, false},
                {"admin", "test", true, false},
                {"config", "test", true, false},
                {"local", "test", true, false},
                {"local", "test", false, false},
            },
        }),
    [](const ::testing::TestParamInfo<ShouldEnforceFastCountAndSizeParams>& info) {
        return info.param.name;
    });

DEATH_TEST(FastCountToStringDeathTest, DiesOnBadFastCountType, "11853100") {
    toString(static_cast<FastCountType>(-1));
}
}  // namespace mongo::collection_validation
