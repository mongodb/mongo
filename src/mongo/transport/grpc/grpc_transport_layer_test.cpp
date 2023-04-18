/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>
#include <vector>

#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {
namespace {

/**
 * TODO SERVER-74021: add documentation for the test fixture.
 */
class GRPCTransportLayerTest : public ServiceContextWithClockSourceMockTest {
public:
    void setUp() override {
        ServiceContextWithClockSourceMockTest::setUp();
        _tl = std::make_unique<GRPCTransportLayer>(getServiceContext(), WireSpec::instance());
    }

    void tearDown() override {
        _tl.reset();
        ServiceContextWithClockSourceMockTest::tearDown();
    }

    GRPCTransportLayer& transportLayer() {
        return *_tl;
    }

private:
    std::unique_ptr<GRPCTransportLayer> _tl;
};

/**
 * Modifies the `ServiceContext` with `PeriodicRunnerMock`, a custom `PeriodicRunner` that maintains
 * a list of all instances of `PeriodicJob` and allows monitoring their internal state. We use this
 * modified runner to test proper initialization and teardown of the idle channel pruner.
 */
class IdleChannelPrunerTest : public GRPCTransportLayerTest {
public:
    class PeriodicRunnerMock : public PeriodicRunner {
    public:
        /**
         * Owns and monitors a `PeriodicJob` by maintaining its observable state (e.g., `kPause`).
         */
        class ControllableJobMock : public ControllableJob {
        public:
            enum class State { kNotSet, kStart, kPause, kResume, kStop };

            explicit ControllableJobMock(PeriodicJob job) : _job(std::move(job)) {}

            void start() override {
                _setState(State::kStart);
            }

            void pause() override {
                _setState(State::kPause);
            }

            void resume() override {
                _setState(State::kResume);
            }

            void stop() override {
                _setState(State::kStop);
            }

            Milliseconds getPeriod() override {
                return _job.interval;
            }

            void setPeriod(Milliseconds ms) override {
                _job.interval = ms;
            }

            bool isStarted() const {
                return _state == State::kStart;
            }

            bool isStopped() const {
                return _state == State::kStop;
            }

        private:
            void _setState(State newState) {
                LOGV2(7401901,
                      "Updating state for a `PeriodicJob`",
                      "jobName"_attr = _job.name,
                      "oldState"_attr = _state,
                      "newState"_attr = newState);
                _state = newState;
            }

            State _state = State::kNotSet;
            PeriodicJob _job;
        };

        PeriodicJobAnchor makeJob(PeriodicJob job) override {
            auto handle = std::make_shared<ControllableJobMock>(std::move(job));
            jobs.push_back(handle);
            return PeriodicJobAnchor{std::move(handle)};
        }

        std::vector<std::shared_ptr<ControllableJobMock>> jobs;
    };

    void setUp() override {
        GRPCTransportLayerTest::setUp();
        getServiceContext()->setPeriodicRunner(std::make_unique<PeriodicRunnerMock>());
    }

    PeriodicRunnerMock* getPeriodicRunnerMock() {
        return static_cast<PeriodicRunnerMock*>(getServiceContext()->getPeriodicRunner());
    }
};

TEST_F(IdleChannelPrunerTest, StartsWithTransportLayer) {
    ASSERT_TRUE(getPeriodicRunnerMock()->jobs.empty());
    ASSERT_OK(transportLayer().start());
    ASSERT_EQ(getPeriodicRunnerMock()->jobs.size(), 1);
    auto& prunerJob = getPeriodicRunnerMock()->jobs[0];
    ASSERT_EQ(prunerJob->getPeriod(), GRPCTransportLayer::kDefaultChannelTimeout);
    ASSERT_TRUE(prunerJob->isStarted());
}

TEST_F(IdleChannelPrunerTest, StopsWithTransportLayer) {
    ASSERT_OK(transportLayer().start());
    transportLayer().shutdown();
    ASSERT_EQ(getPeriodicRunnerMock()->jobs.size(), 1);
    auto& prunerJob = getPeriodicRunnerMock()->jobs[0];
    ASSERT_TRUE(prunerJob->isStopped());
}

}  // namespace
}  // namespace mongo::transport::grpc
