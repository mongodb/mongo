// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_coordinator_mock.h"
#include "mongo/db/index_builds/primary_driven/registry.h"
#include "mongo/db/index_builds/primary_driven/util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version/releases.h"

namespace mongo {
namespace {

// (Generic FCV reference): these validation hooks do not depend on specific versions.
constexpr auto kOriginalVersion = multiversion::GenericFCV::kLastLTS;
constexpr auto kRequestedVersion = multiversion::GenericFCV::kLatest;

class IndexBuildsFCVStepTest : public ServiceContextTest {};
using IndexBuildsFCVStepDeathTest = IndexBuildsFCVStepTest;

DEATH_TEST_F(IndexBuildsFCVStepDeathTest,
             RegistersStepWithoutCoordinator,
             "indexBuildsCoordinator") {
    auto opCtx = makeOperationContext();
    FCVStepRegistry::get(getServiceContext())
        .userCollectionsUassertsForUpgrade(opCtx.get(), kOriginalVersion, kRequestedVersion);
}

TEST_F(IndexBuildsFCVStepTest, ValidatesWithNoIndexBuildsInProgress) {
    IndexBuildsCoordinator::set(getServiceContext(),
                                std::make_unique<IndexBuildsCoordinatorMock>());
    auto opCtx = makeOperationContext();
    auto& steps = FCVStepRegistry::get(getServiceContext());

    ASSERT_DOES_NOT_THROW(
        steps.userCollectionsUassertsForUpgrade(opCtx.get(), kOriginalVersion, kRequestedVersion));
    ASSERT_DOES_NOT_THROW(steps.userCollectionsUassertsForDowngrade(
        opCtx.get(), kRequestedVersion, kOriginalVersion));
}

TEST_F(IndexBuildsFCVStepTest, RejectsInProgressIndexBuildAndRecoversAfterRemoval) {
    IndexBuildsCoordinator::set(getServiceContext(),
                                std::make_unique<IndexBuildsCoordinatorMock>());
    auto opCtx = makeOperationContext();
    auto& steps = FCVStepRegistry::get(getServiceContext());
    auto& registry = index_builds::primary_driven::registry(getServiceContext());
    const auto nss = NamespaceString::createNamespaceString_forTest("test.collection");
    const auto buildUUID = UUID::gen();

    {
        ON_BLOCK_EXIT([&] { registry.remove(buildUUID); });
        registry.add(
            buildUUID,
            nss.dbName(),
            UUID::gen(),
            toIndexBuildInfoVec({BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1")}),
            /*indexBuildIdent=*/boost::none);

        ASSERT_THROWS_CODE(steps.userCollectionsUassertsForUpgrade(
                               opCtx.get(), kOriginalVersion, kRequestedVersion),
                           AssertionException,
                           ErrorCodes::BackgroundOperationInProgressForNamespace);
        ASSERT_THROWS_CODE(steps.userCollectionsUassertsForDowngrade(
                               opCtx.get(), kRequestedVersion, kOriginalVersion),
                           AssertionException,
                           ErrorCodes::BackgroundOperationInProgressForNamespace);
    }

    ASSERT_DOES_NOT_THROW(
        steps.userCollectionsUassertsForUpgrade(opCtx.get(), kOriginalVersion, kRequestedVersion));
    ASSERT_DOES_NOT_THROW(steps.userCollectionsUassertsForDowngrade(
        opCtx.get(), kRequestedVersion, kOriginalVersion));
}

}  // namespace
}  // namespace mongo
