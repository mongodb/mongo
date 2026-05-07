/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/base/string_data.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_control.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

using FCV = multiversion::FeatureCompatibilityVersion;

class FeatureCompatibilityVersionTestFixture : public CatalogTestFixture {
    void setUp() override {
        CatalogTestFixture::setUp();

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(),
                                                                            repl::ReplSettings());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
        // Unit test framework sets FCV to latest. Reset it to test FCV initialization logic.
        serverGlobalParams.mutableFCV.reset();
    }

protected:
    void doStartupFCVSequence(FCV minimumRequiredFCV) {
        Lock::GlobalWrite lock(operationContext());
        FeatureCompatibilityVersion::setIfCleanStartup(
            operationContext(), storageInterface(), minimumRequiredFCV);
        FeatureCompatibilityVersion::initializeForStartup(operationContext());
        FeatureCompatibilityVersion::fassertInitializedAfterStartup(operationContext());
    }
};

using FeatureCompatibilityVersionTestFixtureDeathTest = FeatureCompatibilityVersionTestFixture;
DEATH_TEST_F(FeatureCompatibilityVersionTestFixtureDeathTest, NotInitialized, "invariant") {
    FeatureCompatibilityVersion::fassertInitializedAfterStartup(operationContext());
}

struct StartupFCVSequenceTestParams {
    ClusterRole clusterRole;
    FCV minimumRequiredFCV;
    FCV currentFCV;
    std::string label;
};

class StartupFCVSequenceTestFixture
    : public FeatureCompatibilityVersionTestFixture,
      public testing::WithParamInterface<StartupFCVSequenceTestParams> {};

INSTANTIATE_TEST_SUITE_P(
    StartupFCVSequenceTests,
    StartupFCVSequenceTestFixture,
    testing::ValuesIn({
        StartupFCVSequenceTestParams{ClusterRole::None,
                                     multiversion::GenericFCV::kLastLTS,
                                     multiversion::GenericFCV::kLatest,
                                     "replica_set_prefer_to_start_on_latest"},
        StartupFCVSequenceTestParams{{ClusterRole::ShardServer},
                                     multiversion::GenericFCV::kLastLTS,
                                     multiversion::GenericFCV::kLastLTS,
                                     "shard_server_prefer_to_start_on_last_lts"},
        StartupFCVSequenceTestParams{{ClusterRole::ShardServer, ClusterRole::ConfigServer},
                                     multiversion::GenericFCV::kLastLTS,
                                     multiversion::GenericFCV::kLatest,
                                     "config_server_prefer_to_start_on_latest"},
        StartupFCVSequenceTestParams{{ClusterRole::ShardServer},
                                     multiversion::GenericFCV::kLatest,
                                     multiversion::GenericFCV::kLatest,
                                     "shard_server_minimum_latest_starts_on_latest"},
    }),
    [](const testing::TestParamInfo<StartupFCVSequenceTestParams>& info) {
        return info.param.label;
    });

TEST_P(StartupFCVSequenceTestFixture, StartupFCVSequence) {
    const auto& params = GetParam();
    serverGlobalParams.clusterRole = params.clusterRole;

    doStartupFCVSequence(params.minimumRequiredFCV);

    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, params.currentFCV);
}

struct StartupFCVSequenceTestParamsWithDefault {
    ClusterRole clusterRole;
    FCV defaultStartupFCV;
    FCV minimumRequiredFCV;
    FCV currentFCV;
    std::string label;
};

class StartupFCVSequenceTestFixtureWithDefaultStartupFCV
    : public FeatureCompatibilityVersionTestFixture,
      public testing::WithParamInterface<StartupFCVSequenceTestParamsWithDefault> {};

INSTANTIATE_TEST_SUITE_P(
    StartupFCVSequenceTestsWithDefaultStartupFCV,
    StartupFCVSequenceTestFixtureWithDefaultStartupFCV,
    testing::ValuesIn(
        {StartupFCVSequenceTestParamsWithDefault{
             ClusterRole::None,
             multiversion::GenericFCV::kLastLTS,
             multiversion::GenericFCV::kLastLTS,
             multiversion::GenericFCV::kLastLTS,
             "replica_set_prefer_to_start_on_latest_but_default_startup_fcv_last"},
         StartupFCVSequenceTestParamsWithDefault{
             {ClusterRole::None},
             multiversion::GenericFCV::kLatest,
             multiversion::GenericFCV::kLatest,
             multiversion::GenericFCV::kLatest,
             "default_startup_fcv_latest_overrides_minimum_fcv_last_lts"},
         StartupFCVSequenceTestParamsWithDefault{
             {ClusterRole::None},
             multiversion::GenericFCV::kLastContinuous,
             multiversion::GenericFCV::kLastLTS,
             multiversion::GenericFCV::kLastContinuous,
             "rs_prefer_to_start_on_latest_but_default_startup_fcv_last_continuous"},
         StartupFCVSequenceTestParamsWithDefault{
             {ClusterRole::None},
             multiversion::GenericFCV::kLastLTS,
             multiversion::GenericFCV::kLatest,
             multiversion::GenericFCV::kLatest,
             "default_startup_fcv_last_lts_ignored_if_minimum_fcv_latest"},
         StartupFCVSequenceTestParamsWithDefault{
             {ClusterRole::ShardServer},
             multiversion::GenericFCV::kLastContinuous,
             multiversion::GenericFCV::kLastLTS,
             multiversion::GenericFCV::kLastContinuous,
             "shard_server_prefer_to_start_on_last_lts_but_default_startup_fcv_last_continuous"},
         StartupFCVSequenceTestParamsWithDefault{
             {ClusterRole::ShardServer},
             multiversion::GenericFCV::kLastLTS,
             multiversion::GenericFCV::kLatest,
             multiversion::GenericFCV::kLatest,
             "shard_server_with_default_startup_fcv_last_lts_ignored_if_minimum_fcv_latest"}}),
    [](const testing::TestParamInfo<StartupFCVSequenceTestParamsWithDefault>& info) {
        return info.param.label;
    });


TEST_P(StartupFCVSequenceTestFixtureWithDefaultStartupFCV,
       ReplicaSetCleanStartupDefaultStartupFCVParameterLastLTS) {
    const auto& params = GetParam();

    RAIIServerParameterControllerForTest defaultStartupFCV{"defaultStartupFCV",
                                                           toString(params.defaultStartupFCV)};

    serverGlobalParams.clusterRole = params.clusterRole;

    doStartupFCVSequence(params.minimumRequiredFCV);

    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, params.currentFCV);
}


TEST_F(FeatureCompatibilityVersionTestFixture, ResolveStartNewUpgrade) {
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kLastLTS,
        boost::none,
        lastChangeTimestamp,
        false /* isCleaningServerMetadata */);

    SetFeatureCompatibilityVersion request(multiversion::GenericFCV::kLatest);
    auto result = FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
        operationContext(), request, multiversion::GenericFCV::kLastLTS);

    ASSERT_EQ(result.transitionalVersion, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);
    ASSERT_EQ(result.startPhase, SetFCVPhaseEnum::kStart);
    ASSERT_EQ(result.endPhase, SetFCVPhaseEnum::kComplete);
    ASSERT_GT(result.changeTimestamp, lastChangeTimestamp);
}


TEST_F(FeatureCompatibilityVersionTestFixture, ResolveReturnToOriginalFCVBeforeCommitSucceeds) {
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        SetFCVPhaseEnum::kStart,
        lastChangeTimestamp,
        false /* isCleaningServerMetadata */);

    SetFeatureCompatibilityVersion request(multiversion::GenericFCV::kLastLTS);
    auto result = FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
        operationContext(), request, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_EQ(result.transitionalVersion,
              multiversion::GenericFCV::kDowngradingFromLatestToLastLTS);
    ASSERT_EQ(result.startPhase, SetFCVPhaseEnum::kStart);
    ASSERT_EQ(result.endPhase, SetFCVPhaseEnum::kComplete);
    ASSERT_GT(result.changeTimestamp, lastChangeTimestamp);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ResolveReturnToOriginalFCVDuringCommitFails) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        SetFCVPhaseEnum::kStart,
        Timestamp(10, 10),
        true /* isCleaningServerMetadata */);

    SetFeatureCompatibilityVersion request(multiversion::GenericFCV::kLastLTS);
    ASSERT_THROWS_CODE(
        FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
            operationContext(), request, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest),
        DBException,
        10778001);
}

struct FCVTestParams {
    SetFCVPhaseEnum phase;
    boost::optional<bool> isCleaningServerMetadata;
    bool symmetricFCVEnabled;
    SetFCVPhaseEnum expectedStartPhase;
    SetFCVPhaseEnum expectedEndPhase;
};

class SetFeatureCompatibilityVersionParamTestFixture
    : public FeatureCompatibilityVersionTestFixture,
      public testing::WithParamInterface<FCVTestParams> {};

// Without symmetric FCV, the phase stored in the FCV document is ignored and the transition always
// restarts from kStart with a freshly generated timestamp (legacy "restart from beginning"
// behavior).
INSTANTIATE_TEST_SUITE_P(UpgradingFromDifferentStartingPhasesWithoutSymmetricFCV,
                         SetFeatureCompatibilityVersionParamTestFixture,
                         testing::ValuesIn({
                             FCVTestParams{SetFCVPhaseEnum::kStart,
                                           false,
                                           false,
                                           SetFCVPhaseEnum::kStart,
                                           SetFCVPhaseEnum::kComplete},
                             FCVTestParams{SetFCVPhaseEnum::kPrepare,
                                           false,
                                           false,
                                           SetFCVPhaseEnum::kStart,
                                           SetFCVPhaseEnum::kComplete},
                             FCVTestParams{SetFCVPhaseEnum::kComplete,
                                           true,
                                           false,
                                           SetFCVPhaseEnum::kStart,
                                           SetFCVPhaseEnum::kComplete},
                         }));

// With symmetric FCV, the phase stored in the FCV document is used as the start phase, so the
// transition resumes from where it was interrupted and reuses the existing change timestamp.
INSTANTIATE_TEST_SUITE_P(UpgradingFromDifferentStartingPhasesWithSymmetricFCV,
                         SetFeatureCompatibilityVersionParamTestFixture,
                         testing::ValuesIn({
                             FCVTestParams{SetFCVPhaseEnum::kStart,
                                           false,
                                           true,
                                           SetFCVPhaseEnum::kStart,
                                           SetFCVPhaseEnum::kCommitAddedFeatures},
                             FCVTestParams{SetFCVPhaseEnum::kPrepare,
                                           false,
                                           true,
                                           SetFCVPhaseEnum::kPrepare,
                                           SetFCVPhaseEnum::kCommitAddedFeatures},
                             FCVTestParams{SetFCVPhaseEnum::kComplete,
                                           true,
                                           true,
                                           SetFCVPhaseEnum::kComplete,
                                           SetFCVPhaseEnum::kCommitAddedFeatures},
                             FCVTestParams{SetFCVPhaseEnum::kEnableTargetFeatures,
                                           boost::none,
                                           true,
                                           SetFCVPhaseEnum::kEnableTargetFeatures,
                                           SetFCVPhaseEnum::kCommitAddedFeatures},
                             FCVTestParams{SetFCVPhaseEnum::kCommitAddedFeatures,
                                           boost::none,
                                           true,
                                           SetFCVPhaseEnum::kCommitAddedFeatures,
                                           SetFCVPhaseEnum::kCommitAddedFeatures},
                         }));

TEST_P(SetFeatureCompatibilityVersionParamTestFixture, ResolveResumeInterruptedUpgrade) {
    const auto& params = GetParam();
    RAIIServerParameterControllerForTest symmetricFCV{"featureFlagSymmetricFCV",
                                                      params.symmetricFCVEnabled};
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        params.phase,
        lastChangeTimestamp,
        params.isCleaningServerMetadata /* isCleaningServerMetadata */);

    SetFeatureCompatibilityVersion request(multiversion::GenericFCV::kLatest);
    auto result = FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
        operationContext(), request, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_EQ(result.transitionalVersion, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);
    ASSERT_EQ(result.startPhase, params.expectedStartPhase);
    ASSERT_EQ(result.endPhase, params.expectedEndPhase);
    if (params.symmetricFCVEnabled) {
        ASSERT_EQ(result.changeTimestamp, lastChangeTimestamp);
    } else {
        ASSERT_GT(result.changeTimestamp, lastChangeTimestamp);
    }
}

TEST_F(FeatureCompatibilityVersionTestFixture, CanInitFCVWithIncompleteForegroundIndexBuild) {
    // Create an incomplete index build in the catalog for a collection in the admin database
    {
        const auto nss =
            NamespaceString::createNamespaceString_forTest("admin.incompleteForegroundIndexBuild");
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest{nss,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite},
            MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), &coll};
        auto writableColl = writer.getWritableCollection(operationContext());
        IndexDescriptor desc{IndexNames::BTREE,
                             BSON("v" << 2 << "name"
                                      << "x_1"
                                      << "key" << BSON("x" << 1))};
        ASSERT_OK(writableColl->prepareForIndexBuild(
            operationContext(), &desc, "index-ident", boost::none));
        wuow.commit();
    }

    // Simulate the startup path by closing the catalog and reopening it without reconciling (as
    // that happens after FCV is initialized). This would fail if we tried to initialize the entire
    // admin db as a non-fcv collection is in an invalid state.
    Lock::GlobalLock globalLk(operationContext(), MODE_X);
    catalog::closeCatalog(operationContext());

    auto* storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    storageEngine->loadMDBCatalog(operationContext(), StorageEngine::LastShutdownState::kClean);
    catalog::initializeCollectionCatalog(operationContext(), storageEngine);
    FeatureCompatibilityVersion::initializeForStartup(operationContext());
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       FindFeatureCompatibilityVersionDocumentDoesNotRequireIndex) {
    doStartupFCVSequence(multiversion::GenericFCV::kLatest);

    // Remove the _id index from the collection to verify that FCV lookup doesn't rely on it
    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest{NamespaceString::kServerConfigurationNamespace,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite},
            MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), &coll};
        auto writableColl = writer.getWritableCollection(operationContext());
        ASSERT_TRUE(writableColl->isIndexPresent("_id_"));
        writableColl->removeIndex(operationContext(), "_id_");
        ASSERT_FALSE(writableColl->isIndexPresent("_id_"));
        wuow.commit();
    }

    ASSERT_OK(
        FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(operationContext()));
}

}  // namespace
}  // namespace mongo
