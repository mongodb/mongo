/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/unittest.h"

// TODO(SERVER-119896): Delete this test.
namespace mongo {
namespace {

class IsReplicatedFastCountEnabledTest : public CatalogTestFixture {};

class IsReplicatedFastCountEnabledWithProviderTest : public CatalogTestFixture {
public:
    IsReplicatedFastCountEnabledWithProviderTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count_test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}
};

TEST_F(IsReplicatedFastCountEnabledTest, DisabledWhenFeatureFlagOff) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", false);
    EXPECT_FALSE(isReplicatedFastCountEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, DisabledWhenProviderReturnsFalse) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    EXPECT_TRUE(isReplicatedFastCountEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledWithProviderTest, DisabledWhenFeatureFlagOff) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", false);
    EXPECT_TRUE(isReplicatedFastCountEnabled(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledWithProviderTest, EnabledWhenBothFlagAndProviderOn) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
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

TEST_F(IsReplicatedFastCountEnabledTest, ShouldReadFromSizeStorerWhenProviderOff) {
    EXPECT_TRUE(shouldReadFromSizeStorerForOplog(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledWithProviderTest, ShouldNotReadFromSizeStorerWhenProviderOn) {
    EXPECT_FALSE(shouldReadFromSizeStorerForOplog(operationContext()));
}

TEST_F(IsReplicatedFastCountEnabledTest, ShouldReadFromReplicatedFastCountNoProviderFFOff) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", false);
    std::vector<std::tuple<std::string, std::string>> disallowedCases{
        {"test", "coll1"},    // Normal user collection
        {"config", "coll1"},  // Internal non-local
        {"admin", "coll1"},
        {"local", "coll1"},          // Internal local
        {"config", "transactions"},  // Implicitly replicated
        {"config", "system.preimages"},
        {"config", "image_collection"},
        {"config", "system.profile"},
        {"config",
         std::string{NamespaceString::kReplicatedFastCountStore}},  // Fast count collections
        {"config", std::string{NamespaceString::kReplicatedFastCountStoreTimestamps}},
        {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
         std::string{NamespaceString::kRsOplogNamespace.coll()}},  // Oplog collection
    };

    for (auto&& [dbName, collName] : disallowedCases) {
        EXPECT_FALSE(shouldReadFromReplicatedFastCount(
            operationContext(), NamespaceString::createNamespaceString_forTest(dbName, collName)));
    }
}

TEST_F(IsReplicatedFastCountEnabledTest, ShouldReadFromReplicatedFastCountNoProviderFFOn) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    std::vector<std::tuple<std::string, std::string>> allowedCases{
        {"test", "coll1"},    // Normal user collection
        {"config", "coll1"},  // Internal non-local
        {"admin", "coll1"},
    };
    std::vector<std::tuple<std::string, std::string>> disallowedCases{
        {"local", "coll1"},          // Internal local
        {"config", "transactions"},  // Implicitly replicated
        {"config", "system.preimages"},
        {"config", "image_collection"},
        {"config", "system.profile"},
        {"config",
         std::string{NamespaceString::kReplicatedFastCountStore}},  // Fast count collections
        {"config", std::string{NamespaceString::kReplicatedFastCountStoreTimestamps}},
        {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
         std::string{NamespaceString::kRsOplogNamespace.coll()}},  // Oplog collection
    };

    for (auto&& [dbName, collName] : allowedCases) {
        EXPECT_TRUE(shouldReadFromReplicatedFastCount(
            operationContext(), NamespaceString::createNamespaceString_forTest(dbName, collName)));
    }

    for (auto&& [dbName, collName] : disallowedCases) {
        EXPECT_FALSE(shouldReadFromReplicatedFastCount(
            operationContext(), NamespaceString::createNamespaceString_forTest(dbName, collName)));
    }
}

TEST_F(IsReplicatedFastCountEnabledWithProviderTest,
       ShouldReadFromReplicatedFastCountWithProviderFFOff) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", false);
    std::vector<std::tuple<std::string, std::string>> allowedCases{
        {"test", "coll1"},    // Normal user collection
        {"config", "coll1"},  // Internal non-local
        {"admin", "coll1"},
        {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
         std::string{NamespaceString::kRsOplogNamespace.coll()}},  // Oplog collection
    };
    std::vector<std::tuple<std::string, std::string>> disallowedCases{
        {"local", "coll1"},          // Internal local
        {"config", "transactions"},  // Implicitly replicated
        {"config", "system.preimages"},
        {"config", "image_collection"},
        {"config", "system.profile"},
        {"config",
         std::string{NamespaceString::kReplicatedFastCountStore}},  // Fast count collections
        {"config", std::string{NamespaceString::kReplicatedFastCountStoreTimestamps}},
    };

    for (auto&& [dbName, collName] : allowedCases) {
        EXPECT_TRUE(shouldReadFromReplicatedFastCount(
            operationContext(), NamespaceString::createNamespaceString_forTest(dbName, collName)));
    }

    for (auto&& [dbName, collName] : disallowedCases) {
        EXPECT_FALSE(shouldReadFromReplicatedFastCount(
            operationContext(), NamespaceString::createNamespaceString_forTest(dbName, collName)));
    }
}

TEST_F(IsReplicatedFastCountEnabledWithProviderTest,
       ShouldReadFromReplicatedFastCountWithProviderFFOn) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    std::vector<std::tuple<std::string, std::string>> allowedCases{
        {"test", "coll1"},    // Normal user collection
        {"config", "coll1"},  // Internal non-local
        {"admin", "coll1"},
        {NamespaceString::kRsOplogNamespace.dbName().toString_forTest(),
         std::string{NamespaceString::kRsOplogNamespace.coll()}},  // Oplog collection
    };
    std::vector<std::tuple<std::string, std::string>> disallowedCases{
        {"local", "coll1"},          // Internal local
        {"config", "transactions"},  // Implicitly replicated
        {"config", "system.preimages"},
        {"config", "image_collection"},
        {"config", "system.profile"},
        {"config",
         std::string{NamespaceString::kReplicatedFastCountStore}},  // Fast count collections
        {"config", std::string{NamespaceString::kReplicatedFastCountStoreTimestamps}},
    };

    for (auto&& [dbName, collName] : allowedCases) {
        EXPECT_TRUE(shouldReadFromReplicatedFastCount(
            operationContext(), NamespaceString::createNamespaceString_forTest(dbName, collName)));
    }

    for (auto&& [dbName, collName] : disallowedCases) {
        EXPECT_FALSE(shouldReadFromReplicatedFastCount(
            operationContext(), NamespaceString::createNamespaceString_forTest(dbName, collName)));
    }
}
}  // namespace
}  // namespace mongo
