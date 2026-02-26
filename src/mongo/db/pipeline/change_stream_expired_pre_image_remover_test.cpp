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
#include "mongo/unittest/unittest.h"
#include "mongo/util/mock_periodic_runner.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

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
    ASSERT_FALSE(_preImagesRemover->useReplicatedTruncates_forTest().has_value());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest, FCVSnapshotNotInitialized) {
    setPersistenceProviderWithFlag(false);

    // Deliberately un-initialize FCV snapshot.
    serverGlobalParams.mutableFCV.reset();

    _preImagesRemover->onStepUpComplete(_opCtx.get(), 1 /* term */);
    ASSERT_FALSE(_preImagesRemover->useReplicatedTruncates_forTest().value());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ReplicatedTruncatesSetToTrueWhenUsingReplicatedTruncatesViaPersistenceProvider) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStepUpComplete(_opCtx.get(), 1 /* term */);
    ASSERT_TRUE(_preImagesRemover->useReplicatedTruncates_forTest().value());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ReplicatedTruncatesSetToTrueWhenUsingReplicatedTruncatesViaFeatureFlag) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStepUpComplete(_opCtx.get(), 1 /* term */);
    ASSERT_TRUE(_preImagesRemover->useReplicatedTruncates_forTest().value());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ReplicatedTruncatesSetToFalseWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), true /* isMajority */, false /* isRollback */);
    ASSERT_FALSE(_preImagesRemover->useReplicatedTruncates_forTest().value());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStartedOnStepUpWhenUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStartup(_opCtx.get());
    ASSERT_FALSE(_preImagesRemover->hasStartedPeriodicJob());

    // Step up. This should enable the removal job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_TRUE(_preImagesRemover->hasStartedPeriodicJob());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeNotStartedOnStepUpWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStartup(_opCtx.get());
    ASSERT_FALSE(_preImagesRemover->hasStartedPeriodicJob());

    // Step up. This should not enable the removal job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_FALSE(_preImagesRemover->hasStartedPeriodicJob());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStoppedOnStepDownWhenUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStartup(_opCtx.get());
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_TRUE(_preImagesRemover->hasStartedPeriodicJob());

    // Step down. This should turn off the removal job.
    _preImagesRemover->onStepDown();
    ASSERT_FALSE(_preImagesRemover->hasStartedPeriodicJob());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeNotStoppedOnStepDownWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStartup(_opCtx.get());
    ASSERT_FALSE(_preImagesRemover->hasStartedPeriodicJob());

    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_TRUE(_preImagesRemover->hasStartedPeriodicJob());

    // Step down. This should not turn off the removal job.
    _preImagesRemover->onStepDown();
    ASSERT_TRUE(_preImagesRemover->hasStartedPeriodicJob());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStoppedOnShutdownWhenUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(true);
    setReplicatedTruncatesFeatureFlag(true);

    _preImagesRemover->onStartup(_opCtx.get());

    // Start removal job.
    _preImagesRemover->onStepUpComplete(_opCtx.get(), 42 /*term*/);
    ASSERT_TRUE(_preImagesRemover->hasStartedPeriodicJob());

    // Shut down. This should terminate the removal job.
    _preImagesRemover->onShutdown();
    ASSERT_FALSE(_preImagesRemover->hasStartedPeriodicJob());
}

TEST_F(ChangeStreamExpiredPreImageRemoverTest,
       ExpectRemovalJobToBeStoppedOnShutdownWhenNotUsingReplicatedTruncates) {
    setPersistenceProviderWithFlag(false);
    setReplicatedTruncatesFeatureFlag(false);

    _preImagesRemover->onStartup(_opCtx.get());

    // Start removal job.
    _preImagesRemover->onConsistentDataAvailable(
        _opCtx.get(), false /*isMajority*/, false /*isRollback*/);
    ASSERT_TRUE(_preImagesRemover->hasStartedPeriodicJob());

    // Shut down. This should terminate the removal job.
    _preImagesRemover->onShutdown();
    ASSERT_FALSE(_preImagesRemover->hasStartedPeriodicJob());
}

}  // namespace
}  // namespace mongo
