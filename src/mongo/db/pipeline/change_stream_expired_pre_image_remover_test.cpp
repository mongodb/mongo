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

#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/rss/stub_persistence_provider.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mock_periodic_runner.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using PreImagesRemovalJobContext =
    ChangeStreamExpiredPreImagesRemoverService::PreImagesRemovalJobContext;

/**
 * A simple 'PersistenceProvider' mock that returns a configurable value for
 * 'shouldUseReplicatedTruncates()'.
 */
class UseReplicatedTruncatesPersistenceProvider : public rss::StubPersistenceProvider {
public:
    explicit UseReplicatedTruncatesPersistenceProvider(bool useReplicatedTruncates)
        : _useReplicatedTruncates(useReplicatedTruncates) {}

    bool shouldUseReplicatedTruncates() const override {
        return _useReplicatedTruncates;
    }

private:
    bool const _useReplicatedTruncates;
};

class ChangeStreamExpiredPreImageRemoverTest : public ServiceContextTest {
public:
    ChangeStreamExpiredPreImageRemoverTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(false /* shouldSetupTL */)) {

        getServiceContext()->setPeriodicRunner(std::make_unique<MockPeriodicRunner>());

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        _opCtx = makeOperationContext();
        _preImagesRemover = std::make_unique<ChangeStreamExpiredPreImagesRemoverService>();
    }

    void setReplicatedTruncatesFeatureFlag(bool useReplicatedTruncates) {
        _featureFlag = std::make_unique<RAIIServerParameterControllerForTest>(
            "featureFlagUseReplicatedTruncatesForDeletions", useReplicatedTruncates);
    }

    void setPersistenceProviderWithFlag(bool useReplicatedTruncates) {
        rss::ReplicatedStorageService::get(_opCtx.get())
            .setPersistenceProvider(std::make_unique<UseReplicatedTruncatesPersistenceProvider>(
                useReplicatedTruncates));
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ChangeStreamExpiredPreImagesRemoverService> _preImagesRemover;

    std::unique_ptr<RAIIServerParameterControllerForTest> _featureFlag;
};

TEST_F(ChangeStreamExpiredPreImageRemoverTest, ReplicatedTruncatesNotPopulatedInitially) {
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest, FCVSnapshotNotInitialized) {
    setPersistenceProviderWithFlag(false);

    // Deliberately un-initialize FCV snapshot.
    serverGlobalParams.mutableFCV.reset();

    _preImagesRemover->onStepUpComplete(_opCtx.get(), 1 /* term */);
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ReplicatedTruncatesSetToTrueWhenUsingReplicatedTruncatesViaPersistenceProvider) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStepUpComplete(_opCtx.get(), 1 /* term */);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ReplicatedTruncatesSetToTrueWhenUsingReplicatedTruncatesViaFeatureFlag) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStepUpComplete(_opCtx.get(), 1 /* term */);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ReplicatedTruncatesSetToFalseWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), true /* isMajority */, false /* isRollback */);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStartedOnStepUpWhenUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStartup(_opCtx.get());
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());

    // Step up. This should enable the removal job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeNotStartedOnStepUpWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStartup(_opCtx.get());
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());

    // Step up. This should not enable the removal job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStoppedOnStepDownWhenUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStartup(_opCtx.get());
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());

    // Step down. This should turn off the removal job.
    _preImagesRemover->onStepDown();
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeNotStoppedOnStepDownWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStartup(_opCtx.get());
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());

    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Step down. This should not turn off the removal job.
    _preImagesRemover->onStepDown();
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStoppedOnShutdownWhenUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStartup(_opCtx.get());

    // Start removal job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());

    // Shut down. This should terminate the removal job.
    _preImagesRemover->onShutdown();
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStoppedOnShutdownWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStartup(_opCtx.get());

    // Start removal job.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Shut down. This should terminate the removal job.
    _preImagesRemover->onShutdown();
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectReplicatedTruncatesToBeUsedOnPrimaryOnFCVUpgradeFromLastLTSToLatestWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastLTS);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Make ourselves the primary. The job should still be running.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);

    // Flush the current job in the job runner because otherwise the mock is not prepared to start
    // more than one job in its entire lifetime.
    static_cast<MockPeriodicRunner*>(getServiceContext()->getPeriodicRunner())->resetJob();
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should be running, but now using replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 2, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnPrimaryOnFCVUpgradeFromLastLTSToLatestWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastLTS);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Make ourselves the primary. The same job should still be running.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);
    static_cast<MockPeriodicRunner*>(getServiceContext()->getPeriodicRunner())->resetJob();
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be still running, and still not use replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVUpgradeFromLastLTSToLatestWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastLTS);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should not be running on secondary.
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVUpgradeFromLastLTSToLatestWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastLTS);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be still running, and still not use replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectReplicatedTruncatesToBeUsedOnPrimaryOnFCVUpgradeFromLastContinuousToLatestWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastContinuous);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Make ourselves the primary. The job should still be running.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);

    // Flush the current job in the job runner because otherwise the mock is not prepared to start
    // more than one job in its entire lifetime.
    static_cast<MockPeriodicRunner*>(getServiceContext()->getPeriodicRunner())->resetJob();
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should be running, but now using replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 2, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnPrimaryOnFCVUpgradeFromLastContinuousToLatestWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastContinuous);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Make ourselves the primary. The same job should still be running.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);
    static_cast<MockPeriodicRunner*>(getServiceContext()->getPeriodicRunner())->resetJob();
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be still running, and still not use replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVUpgradeFromLastContinuousToLatestWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastContinuous);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should not be running on secondary.
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVUpgradeFromLastContinuousToLatestWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLastContinuous);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to latest.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLatest);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be still running, and still not use replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectReplicatedTruncatesToBeUsedOnPrimaryOnFCVDowngradeFromLatestToLastLTSWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Make ourselves the primary and start the job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());

    // Downgrade FCV. Flush the current job in the job runner because otherwise the mock is not
    // prepared to start more than one job in its entire lifetime.
    static_cast<MockPeriodicRunner*>(getServiceContext()->getPeriodicRunner())->resetJob();

    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastLTS);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should have been restarted, now using local truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 2, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnPrimaryOnFCVDowngradeFromLatestToLastLTSWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start the job.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to last LTS.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastLTS);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be running.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVDowngradeFromLatestToLastLTSWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Job should not be running on a secondary.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());

    // Change FCV to last LTS.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastLTS);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should have been started, using local truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVDowngradeFromLatestToLastLTSWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to last LTS.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastLTS);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be still running, and still not use replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectReplicatedTruncatesToBeUsedOnPrimaryOnFCVDowngradeFromLatestToLastContinuousWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Make ourselves the primary and start the job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = true}),
              _preImagesRemover->getJobContext_forTest());

    // Downgrade FCV. Flush the current job in the job runner because otherwise the mock is not
    // prepared to start more than one job in its entire lifetime.
    static_cast<MockPeriodicRunner*>(getServiceContext()->getPeriodicRunner())->resetJob();

    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastContinuous);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should have been restarted, now using local truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 2, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnPrimaryOnFCVDowngradeFromLatestToLastContinuousWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start the job.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to last continuous.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastContinuous);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be running.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVDowngradeFromLatestToLastContinuousWhenFeatureFlagIsEnabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    // Job should not be running on a secondary.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ(boost::none, _preImagesRemover->getJobContext_forTest());

    // Change FCV to last continuous.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastContinuous);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Job should have been started, using local truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

TEST_F(
    ChangeStreamExpiredPreImageRemoverTest,
    ExpectLocalTruncatesToBeUsedOnNonPrimaryOnFCVDowngradeFromLatestToLastContinuousWhenFeatureFlagIsDisabled) {
    // (Generic FCV reference): feature flag test
    unittest::EnsureFCV scopedFCV(multiversion::GenericFCV::kLatest);

    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    // Start removal job. This should not use replicated truncates.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());

    // Change FCV to last continuous.
    // (Generic FCV reference): feature flag test
    ServerGlobalParams::FCVSnapshot newFcvSnapshot(multiversion::GenericFCV::kLastContinuous);
    _preImagesRemover->onFCVChange(_opCtx.get(), newFcvSnapshot);

    // Same job should be still running, and still not use replicated truncates.
    ASSERT_EQ((PreImagesRemovalJobContext{.id = 1, .usesReplicatedTruncates = false}),
              _preImagesRemover->getJobContext_forTest());
}

}  // namespace
}  // namespace mongo
