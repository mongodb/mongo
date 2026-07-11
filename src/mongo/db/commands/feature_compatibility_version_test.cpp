// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/op_observer/fcv_op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_control.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

using FCV = multiversion::FeatureCompatibilityVersion;

class FeatureCompatibilityVersionTestFixture : public CatalogTestFixture {
protected:
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

    unittest::ServerParameterGuard defaultStartupFCV{"defaultStartupFCV",
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
    ASSERT_EQ(result.endPhase, SetFCVPhaseEnum::kCommitAddedFeatures);
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
    ASSERT_EQ(result.endPhase, SetFCVPhaseEnum::kCommitAddedFeatures);
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
    unittest::ServerParameterGuard symmetricFCV{"featureFlagSymmetricFCV",
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

struct UpdateDocumentPreviousVersionTestParams {
    SetFCVPhaseEnum phase;
    bool expectPreviousVersion;
    std::string label;
};

class UpdateDocumentPreviousVersionTestFixture
    : public FeatureCompatibilityVersionTestFixture,
      public testing::WithParamInterface<UpdateDocumentPreviousVersionTestParams> {};

INSTANTIATE_TEST_SUITE_P(
    UpdateDocumentPreviousVersionTests,
    UpdateDocumentPreviousVersionTestFixture,
    testing::ValuesIn({
        UpdateDocumentPreviousVersionTestParams{
            SetFCVPhaseEnum::kEnableTargetFeatures,
            true,
            "writes_previous_version_at_enable_target_features"},
        UpdateDocumentPreviousVersionTestParams{SetFCVPhaseEnum::kCommitAddedFeatures,
                                                true,
                                                "writes_previous_version_at_commit_added_features"},
        UpdateDocumentPreviousVersionTestParams{
            SetFCVPhaseEnum::kStart, false, "does_not_write_on_upgrade_previous_version_at_start"},
    }),
    [](const testing::TestParamInfo<UpdateDocumentPreviousVersionTestParams>& info) {
        return info.param.label;
    });

TEST_P(UpdateDocumentPreviousVersionTestFixture, UpdateDocumentPreviousVersion) {
    const auto& params = GetParam();
    unittest::ServerParameterGuard symmetricFCV{"featureFlagSymmetricFCV", true};
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    const Timestamp ts =
        VectorClockMutable::get(operationContext())->tickClusterTime(1).asTimestamp();
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        params.phase,
        ts,
        params.phase >= SetFCVPhaseEnum::kEnableTargetFeatures /* setIsCleaningServerMetadata */);

    auto docResult =
        FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(operationContext());
    ASSERT_OK(docResult);
    auto parsedDoc = FeatureCompatibilityVersionDocument::parse(docResult.getValue());
    if (params.expectPreviousVersion) {
        ASSERT_TRUE(parsedDoc.getPreviousVersion().has_value());
        ASSERT_EQ(*parsedDoc.getPreviousVersion(), multiversion::GenericFCV::kLastLTS);
    } else {
        ASSERT_FALSE(parsedDoc.getPreviousVersion().has_value());
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

TEST_F(FeatureCompatibilityVersionTestFixture, UpdateDocumentClearsPreviousVersionOnFinalize) {
    unittest::ServerParameterGuard symmetricFCV{"featureFlagSymmetricFCV", true};
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    const Timestamp ts =
        VectorClockMutable::get(operationContext())->tickClusterTime(1).asTimestamp();
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kLatest,
        boost::none /* phase */,
        ts,
        boost::none /* setIsCleaningServerMetadata */);

    auto docResult =
        FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(operationContext());
    ASSERT_OK(docResult);
    auto parsedDoc = FeatureCompatibilityVersionDocument::parse(docResult.getValue());
    ASSERT_FALSE(parsedDoc.getPreviousVersion().has_value());
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       UpdateDocumentDoesNotWritePreviousVersionWhenSymmetricFCVDisabled) {
    unittest::ServerParameterGuard symmetricFCV{"featureFlagSymmetricFCV", false};
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    const Timestamp ts =
        VectorClockMutable::get(operationContext())->tickClusterTime(1).asTimestamp();
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        SetFCVPhaseEnum::kPrepare,
        ts,
        boost::none /* setIsCleaningServerMetadata */);

    auto docResult =
        FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(operationContext());
    ASSERT_OK(docResult);
    auto parsedDoc = FeatureCompatibilityVersionDocument::parse(docResult.getValue());
    ASSERT_FALSE(parsedDoc.getPreviousVersion().has_value());
}
// Helpers for the FeatureCompatibilityVersionParameter::append() tests below.

BSONObj appendFcvParameter(OperationContext* opCtx) {
    auto* sp = ServerParameterSet::getNodeParameterSet()->get(multiversion::kParameterName);
    BSONObjBuilder b;
    sp->append(opCtx, &b, sp->name(), boost::none);
    return b.obj().getObjectField(sp->name()).getOwned();
}

void seedFcvDocAtPhase(OperationContext* opCtx,
                       FCV version,
                       boost::optional<SetFCVPhaseEnum> phase) {
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        opCtx,
        version,
        phase,
        Timestamp(1, 1),
        phase &&
            *phase >= SetFCVPhaseEnum::kEnableTargetFeatures /* setIsCleaningServerMetadata */);
    // The FCV op observer that normally mirrors disk writes to in-memory FCV does not run in
    // unit tests. Replicate its effect here so callers see consistent in-memory state.
    auto docResult = FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(opCtx);
    ASSERT_OK(docResult.getStatus());
    serverGlobalParams.mutableFCV.setVersion(
        uassertStatusOK(FeatureCompatibilityVersionParser::parse(docResult.getValue())));
}

std::string_view fcvStr(FCV v) {
    return FeatureCompatibilityVersionParser::serializeVersionForFcvString(v);
}

struct AppendFCVFormatTestParams {
    // The version to seed on disk. A non-transitional version with an unset phase yields a
    // steady-state document; a transitional version paired with a phase yields a transitional one.
    FCV seedVersion;
    boost::optional<SetFCVPhaseEnum> phase;
    std::function<BSONObj()> expectedDoc;
    std::string label;
};

class AppendFCVFormatTestFixture : public FeatureCompatibilityVersionTestFixture,
                                   public testing::WithParamInterface<AppendFCVFormatTestParams> {};

INSTANTIATE_TEST_SUITE_P(
    AppendFCVFormatTests,
    AppendFCVFormatTestFixture,
    testing::ValuesIn({
        AppendFCVFormatTestParams{multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
                                  SetFCVPhaseEnum::kEnableTargetFeatures,
                                  []() {
                                      return BSON("version"
                                                  << fcvStr(multiversion::GenericFCV::kLatest)
                                                  << "targetVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLatest)
                                                  << "previousVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLastLTS));
                                  },
                                  "upgrade_enable_target_features"},
        AppendFCVFormatTestParams{multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
                                  SetFCVPhaseEnum::kCommitAddedFeatures,
                                  []() {
                                      return BSON("version"
                                                  << fcvStr(multiversion::GenericFCV::kLatest)
                                                  << "targetVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLatest)
                                                  << "previousVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLastLTS));
                                  },
                                  "upgrade_commit_added_features"},
        AppendFCVFormatTestParams{multiversion::GenericFCV::kDowngradingFromLatestToLastLTS,
                                  SetFCVPhaseEnum::kEnableTargetFeatures,
                                  []() {
                                      return BSON("version"
                                                  << fcvStr(multiversion::GenericFCV::kLastLTS)
                                                  << "targetVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLastLTS)
                                                  << "previousVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLatest));
                                  },
                                  "downgrade_enable_target_features"},
        AppendFCVFormatTestParams{multiversion::GenericFCV::kDowngradingFromLatestToLastLTS,
                                  SetFCVPhaseEnum::kCommitAddedFeatures,
                                  []() {
                                      return BSON("version"
                                                  << fcvStr(multiversion::GenericFCV::kLastLTS)
                                                  << "targetVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLastLTS)
                                                  << "previousVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLatest));
                                  },
                                  "downgrade_commit_added_features"},
        AppendFCVFormatTestParams{multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
                                  SetFCVPhaseEnum::kStart,
                                  []() {
                                      return BSON("version"
                                                  << fcvStr(multiversion::GenericFCV::kLastLTS)
                                                  << "targetVersion"
                                                  << fcvStr(multiversion::GenericFCV::kLatest));
                                  },
                                  "upgrade_start"},
        // A fully steady-state FCV (no transition in progress) reports just {version}.
        AppendFCVFormatTestParams{
            multiversion::GenericFCV::kLatest,
            boost::none,
            []() { return BSON("version" << fcvStr(multiversion::GenericFCV::kLatest)); },
            "steady_state"},
    }),
    [](const testing::TestParamInfo<AppendFCVFormatTestParams>& info) { return info.param.label; });

TEST_P(AppendFCVFormatTestFixture, AppendEmitsExpectedFormat) {
    const auto& params = GetParam();
    unittest::ServerParameterGuard symmetricFCV{"featureFlagSymmetricFCV", true};
    serverGlobalParams.clusterRole = ClusterRole::None;
    doStartupFCVSequence(multiversion::GenericFCV::kLatest);
    seedFcvDocAtPhase(operationContext(), params.seedVersion, params.phase);

    auto fcv = appendFcvParameter(operationContext());

    ASSERT_BSONOBJ_EQ(fcv, params.expectedDoc());
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

// ---- InitializeForStartup tests ----

static FeatureCompatibilityVersionDocument getFCVDocument() {
    FeatureCompatibilityVersionDocument doc;
    serverGlobalParams.mutableFCV.withAcquiredFCVDocument(
        [&](const FeatureCompatibilityVersionDocument* fcvDoc) {
            ASSERT(fcvDoc);
            doc = *fcvDoc;
        });
    return doc;
}

struct StartupFCVScenario {
    std::string name;
    FCV targetVersion;  // Steady or transitional FCV to write before startup.
    boost::optional<SetFCVPhaseEnum> phase;
    FCV expectedVersion;
};

class InitializeForStartupTest : public FeatureCompatibilityVersionTestFixture,
                                 public testing::WithParamInterface<StartupFCVScenario> {};

INSTANTIATE_TEST_SUITE_P(
    InitializeForStartup,
    InitializeForStartupTest,
    testing::ValuesIn(std::vector<StartupFCVScenario>{
        {"SteadyState",
         multiversion::GenericFCV::kLatest,
         boost::none,
         multiversion::GenericFCV::kLatest},
        {"EarlyUpgrade",
         multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
         SetFCVPhaseEnum::kStart,
         multiversion::GenericFCV::kUpgradingFromLastLTSToLatest},
        {"LateUpgrade",
         multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
         SetFCVPhaseEnum::kEnableTargetFeatures,
         multiversion::GenericFCV::kLatest},
        {"Downgrade",
         multiversion::GenericFCV::kDowngradingFromLatestToLastLTS,
         SetFCVPhaseEnum::kStart,
         multiversion::GenericFCV::kDowngradingFromLatestToLastLTS},
    }),
    [](const testing::TestParamInfo<InitializeForStartupTest::ParamType>& info) {
        return info.param.name;
    });

TEST_P(InitializeForStartupTest, InitializeForStartupSetsFCVDocument) {
    const auto& s = GetParam();

    doStartupFCVSequence(multiversion::GenericFCV::kLatest);

    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(), s.targetVersion, s.phase, boost::none, boost::none);

    serverGlobalParams.mutableFCV.reset();

    {
        Lock::GlobalWrite lk(operationContext());
        FeatureCompatibilityVersion::initializeForStartup(operationContext());
    }

    auto snapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    ASSERT_EQ(snapshot.getVersion(), s.expectedVersion);

    auto swOnDiskFCVDoc =
        FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(operationContext());
    ASSERT_OK(swOnDiskFCVDoc.getStatus());

    auto fcvDoc = getFCVDocument();
    ASSERT_BSONOBJ_EQ(swOnDiskFCVDoc.getValue(), fcvDoc.toBSON());
}

TEST_F(InitializeForStartupTest, InitializeForStartupNoDocument) {
    Lock::GlobalWrite lk(operationContext());
    FeatureCompatibilityVersion::initializeForStartup(operationContext());

    auto snapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    ASSERT_FALSE(snapshot.isVersionInitialized());
}

// ---- FCVOpObserver E2E test ----

class FCVOpObserverE2ETest : public FeatureCompatibilityVersionTestFixture {
public:
    void setUp() override {
        FeatureCompatibilityVersionTestFixture::setUp();
        auto* registry = checked_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        registry->addObserver(std::make_unique<FcvOpObserver>());
    }
};

TEST_F(FCVOpObserverE2ETest, SetsFCVDocumentOnDocumentWrite) {
    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        SetFCVPhaseEnum::kEnableTargetFeatures,
        boost::none,
        boost::none);

    auto snapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    ASSERT_EQ(snapshot.getVersion(), multiversion::GenericFCV::kLatest);

    auto fcvDoc = getFCVDocument();
    ASSERT_EQ(fcvDoc.getTargetVersion(), multiversion::GenericFCV::kLatest);
    ASSERT_EQ(fcvDoc.getPreviousVersion(), multiversion::GenericFCV::kLastLTS);
}

}  // namespace
}  // namespace mongo
