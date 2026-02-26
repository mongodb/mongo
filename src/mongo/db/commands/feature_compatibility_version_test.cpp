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

TEST_F(FeatureCompatibilityVersionTestFixture, ReplicaSetCleanStartup) {
    serverGlobalParams.clusterRole = ClusterRole::None;

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    // Replica sets prefer to start on latest.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLatest);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ShardServerCleanStartupMinimumLastLTS) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    // ShardServers prefer to start on lastLTS.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLastLTS);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ConfigServerCleanStartupMinimumLastLTS) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    // ConfigServer's prefer to start on latest.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLatest);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ShardServerCleanStartupMinimumLatest) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLatest);

    // ShardServers prefer to start on lastLTS, but we specified minimum latest.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLatest);
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       ReplicaSetCleanStartupDefaultStartupFCVParameterLastLTS) {
    RAIIServerParameterControllerForTest defaultStartupFCV{
        "defaultStartupFCV", toString(multiversion::GenericFCV::kLastLTS)};

    serverGlobalParams.clusterRole = ClusterRole::None;

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    // Replica sets prefer to start on latest, but defaultStartupFCV specified lastLTS.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLastLTS);
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       ReplicaSetCleanStartupDefaultStartupFCVParameterLatest) {
    RAIIServerParameterControllerForTest defaultStartupFCV{
        "defaultStartupFCV", toString(multiversion::GenericFCV::kLatest)};

    serverGlobalParams.clusterRole = ClusterRole::None;

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    // defaultStartupFCV specified latest which overrides the minimum FCV of lastLTS.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLatest);
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       ReplicaSetCleanStartupDefaultStartupFCVParameterLastContinuous) {
    RAIIServerParameterControllerForTest defaultStartupFCV{
        "defaultStartupFCV", toString(multiversion::GenericFCV::kLastContinuous)};

    serverGlobalParams.clusterRole = ClusterRole::None;

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    // Replica sets prefer to start on latest, but defaultStartupFCV specified lastContinuous.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLastContinuous);
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       ReplicaSetCleanStartupDefaultStartupFCVParameterWithHigherMinimumFCV) {
    // defaultStartupFCV should be ignored if there's a higher required minimum FCV.
    RAIIServerParameterControllerForTest defaultStartupFCV{
        "defaultStartupFCV", toString(multiversion::GenericFCV::kLastLTS)};

    serverGlobalParams.clusterRole = ClusterRole::None;

    doStartupFCVSequence(multiversion::GenericFCV::kLatest);

    // defaultStartupFCV specified lastLTS, but minimum was latest.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLatest);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ShardServerCleanStartupDefaultStartupFCVParameter) {
    // ShardServers prefer lastLTS, but can be overriden.
    RAIIServerParameterControllerForTest defaultStartupFCV{
        "defaultStartupFCV", toString(multiversion::GenericFCV::kLastContinuous)};

    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);

    // ShardServers prefer to start on lastLTS, but defaultStartupFCV specified lastContinuous.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLastContinuous);
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       ShardServerCleanStartupDefaultStartupFCVParameterWithHigherMinimumFCV) {
    // defaultStartupFCV should be ignored if there's a higher required minimum FCV.
    RAIIServerParameterControllerForTest defaultStartupFCV{
        "defaultStartupFCV", toString(multiversion::GenericFCV::kLastLTS)};

    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    doStartupFCVSequence(multiversion::GenericFCV::kLatest);

    // ShardServers with defaultStartupFCV set to lastLTS, but a minimum FCV of latest.
    const auto currentFcv =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ASSERT_EQ(currentFcv, multiversion::GenericFCV::kLatest);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ResolveStartNewUpgrade) {
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kLastLTS,
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

TEST_F(FeatureCompatibilityVersionTestFixture, ResolveResumeInterruptedUpgradeBeforeCommit) {
    RAIIServerParameterControllerForTest symmetricFCV{"featureFlagSymmetricFCV", true};
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        lastChangeTimestamp,
        false /* isCleaningServerMetadata */);

    SetFeatureCompatibilityVersion request(multiversion::GenericFCV::kLatest);
    auto result = FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
        operationContext(), request, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_EQ(result.transitionalVersion, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);
    ASSERT_EQ(result.startPhase, SetFCVPhaseEnum::kStart);
    ASSERT_EQ(result.endPhase, SetFCVPhaseEnum::kComplete);
    ASSERT_EQ(result.changeTimestamp, lastChangeTimestamp);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ResolveResumeInterruptedUpgradeDuringCommit) {
    RAIIServerParameterControllerForTest symmetricFCV{"featureFlagSymmetricFCV", true};
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
        lastChangeTimestamp,
        true /* isCleaningServerMetadata */);

    SetFeatureCompatibilityVersion request(multiversion::GenericFCV::kLatest);
    auto result = FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
        operationContext(), request, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_EQ(result.transitionalVersion, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);
    ASSERT_EQ(result.startPhase, SetFCVPhaseEnum::kComplete);
    ASSERT_EQ(result.endPhase, SetFCVPhaseEnum::kComplete);
    ASSERT_EQ(result.changeTimestamp, lastChangeTimestamp);
}

TEST_F(FeatureCompatibilityVersionTestFixture, ResolveReturnToOriginalFCVBeforeCommitSucceeds) {
    const Timestamp lastChangeTimestamp =
        VectorClockMutable::get(operationContext())->tickClusterTime(2).asTimestamp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    doStartupFCVSequence(multiversion::GenericFCV::kLastLTS);
    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
        operationContext(),
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest,
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
        Timestamp(10, 10),
        true /* isCleaningServerMetadata */);

    SetFeatureCompatibilityVersion request(multiversion::GenericFCV::kLastLTS);
    ASSERT_THROWS_CODE(
        FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
            operationContext(), request, multiversion::GenericFCV::kUpgradingFromLastLTSToLatest),
        DBException,
        10778001);
}

}  // namespace
}  // namespace mongo
