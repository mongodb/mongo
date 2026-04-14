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
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
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
    bool isCleaningServerMetadata;
};

class SetFeatureCompatibilityVersionParamTestFixture
    : public FeatureCompatibilityVersionTestFixture,
      public testing::WithParamInterface<FCVTestParams> {};

INSTANTIATE_TEST_SUITE_P(UpgradingFromDifferentStartingPhases,
                         SetFeatureCompatibilityVersionParamTestFixture,
                         testing::ValuesIn({
                             FCVTestParams{SetFCVPhaseEnum::kStart, false},
                             FCVTestParams{SetFCVPhaseEnum::kPrepare, false},
                             FCVTestParams{SetFCVPhaseEnum::kComplete, true},
                         }));

TEST_P(SetFeatureCompatibilityVersionParamTestFixture, ResolveResumeInterruptedUpgrade) {
    RAIIServerParameterControllerForTest symmetricFCV{"featureFlagSymmetricFCV", true};
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    const auto& params = GetParam();

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
    ASSERT_EQ(result.startPhase, params.phase);
    ASSERT_EQ(result.endPhase, SetFCVPhaseEnum::kComplete);
    ASSERT_EQ(result.changeTimestamp, lastChangeTimestamp);
}


}  // namespace
}  // namespace mongo
