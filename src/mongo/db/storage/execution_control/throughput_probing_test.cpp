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

#include "mongo/db/storage/execution_control/throughput_probing.h"
#include "mongo/db/storage/execution_control/throughput_probing_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo::execution_control {
namespace throughput_probing {
namespace {

TEST(ThroughputProbingParameterTest, InitialConcurrency) {
    ASSERT_OK(validateInitialConcurrency(gMinConcurrency, {}));
    ASSERT_OK(validateInitialConcurrency(gMaxConcurrency.load(), {}));
    ASSERT_NOT_OK(validateInitialConcurrency(gMinConcurrency - 1, {}));
    ASSERT_NOT_OK(validateInitialConcurrency(gMaxConcurrency.load() + 1, {}));
}

TEST(ThroughputProbingParameterTest, MinConcurrency) {
    ASSERT_OK(validateMinConcurrency(5, {}));
    ASSERT_OK(validateMinConcurrency(gMaxConcurrency.load(), {}));
    ASSERT_NOT_OK(validateMinConcurrency(0, {}));
    ASSERT_NOT_OK(validateMinConcurrency(gMaxConcurrency.load() + 1, {}));
}

TEST(ThroughputProbingParameterTest, MaxConcurrency) {
    ASSERT_OK(validateMaxConcurrency(gMinConcurrency, {}));
    ASSERT_OK(validateMaxConcurrency(256, {}));
    ASSERT_NOT_OK(validateMaxConcurrency(gMinConcurrency - 1, {}));
}

}  // namespace
}  // namespace throughput_probing

namespace {

class MockPeriodicJob : public PeriodicRunner::ControllableJob {
public:
    explicit MockPeriodicJob(PeriodicRunner::PeriodicJob job) : _job(std::move(job)) {}

    void start() override {}
    void pause() override {}
    void resume() override {}
    void stop() override {}

    Milliseconds getPeriod() override {
        return _job.interval;
    }

    void setPeriod(Milliseconds period) override {
        _job.interval = period;
    }

    void run(Client* client) {
        _job.job(client);
    }

private:
    PeriodicRunner::PeriodicJob _job;
};

class MockPeriodicRunner : public PeriodicRunner {
public:
    JobAnchor makeJob(PeriodicJob job) override {
        invariant(!_job);
        auto mockJob = std::make_shared<MockPeriodicJob>(std::move(job));
        _job = mockJob;
        return JobAnchor{std::move(mockJob)};
    }

    void run(Client* client) {
        invariant(_job);
        _job->run(client);
    }

private:
    std::shared_ptr<MockPeriodicJob> _job;
};

namespace {
TickSourceMock<Microseconds>* initTickSource(ServiceContext* svcCtx) {
    auto mockTickSource = std::make_unique<TickSourceMock<Microseconds>>();
    auto tickSourcePtr = mockTickSource.get();
    svcCtx->setTickSource(std::move(mockTickSource));
    return tickSourcePtr;
}
}  // namespace

class ThroughputProbingTest : public unittest::Test {
protected:
    explicit ThroughputProbingTest(int32_t size = 64)
        : _runner([svcCtx = _svcCtx.get()] {
              auto runner = std::make_unique<MockPeriodicRunner>();
              auto runnerPtr = runner.get();
              svcCtx->setPeriodicRunner(std::move(runner));
              return runnerPtr;
          }()),
          _tickSource(initTickSource(_svcCtx.get())),
          _throughputProbing([&]() -> ThroughputProbing {
              throughput_probing::gInitialConcurrency = size;
              return {_svcCtx.get(), &_readTicketHolder, &_writeTicketHolder, Milliseconds{1}};
          }()) {
        // We need to advance the ticks to something other than zero, since that is used to
        // determine the if we're in the first iteration or not.
        _tick();

        // First loop is a no-op and initializes state.
        _run();
    }

    void _run() {
        _runner->run(_client.get());
    }

    void _tick() {
        _tickSource->advance(Microseconds(1000));
    }

    ServiceContext::UniqueServiceContext _svcCtx = ServiceContext::make();
    ServiceContext::UniqueClient _client = _svcCtx->makeClient("ThroughputProbingTest");

    MockPeriodicRunner* _runner;
    MockTicketHolder _readTicketHolder;
    MockTicketHolder _writeTicketHolder;
    TickSourceMock<Microseconds>* _tickSource;
    ThroughputProbing _throughputProbing;
};

using namespace throughput_probing;

class ThroughputProbingMaxConcurrencyTest : public ThroughputProbingTest {
protected:
    ThroughputProbingMaxConcurrencyTest() : ThroughputProbingTest(gMaxConcurrency.load()) {}
};

class ThroughputProbingMinConcurrencyTest : public ThroughputProbingTest {
protected:
    ThroughputProbingMinConcurrencyTest() : ThroughputProbingTest(gMinConcurrency) {}
};

TEST_F(ThroughputProbingTest, ProbeUpSucceeds) {
    // Tickets are exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setUsed(size);
    _readTicketHolder.setUsed(size - 1);
    _readTicketHolder.setNumFinishedProcessing(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_GT(_readTicketHolder.outof(), size);
    ASSERT_GT(_writeTicketHolder.outof(), size);

    // Throughput inreases.
    size = _readTicketHolder.outof();
    _readTicketHolder.setNumFinishedProcessing(3);
    _tick();

    // Probing up succeeds; the new value is promoted to stable.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingTest, ProbeUpFails) {
    // Tickets are exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setUsed(size);
    _readTicketHolder.setUsed(size - 1);
    _readTicketHolder.setNumFinishedProcessing(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_GT(_readTicketHolder.outof(), size);
    ASSERT_GT(_writeTicketHolder.outof(), size);

    // Throughput does not increase.
    _readTicketHolder.setNumFinishedProcessing(2);
    _tick();

    // Probing up fails since throughput did not increase. Return to stable.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingTest, ProbeDownSucceeds) {
    // Tickets are not exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setUsed(size - 1);
    _readTicketHolder.setNumFinishedProcessing(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), size);
    ASSERT_LT(_writeTicketHolder.outof(), size);

    // Throughput increases.
    size = _readTicketHolder.outof();
    _readTicketHolder.setNumFinishedProcessing(3);
    _tick();

    // Probing down succeeds; the new value is promoted to stable.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingTest, ProbeDownFails) {
    // Tickets are not exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setUsed(size - 1);
    _readTicketHolder.setNumFinishedProcessing(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), size);
    ASSERT_LT(_writeTicketHolder.outof(), size);

    // Throughput does not increase.
    _readTicketHolder.setNumFinishedProcessing(2);
    _tick();

    // Probing down fails since throughput did not increase. Return back to stable.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingMaxConcurrencyTest, NoProbeUp) {
    // Tickets are exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setUsed(size);
    _readTicketHolder.setUsed(size - 1);
    _readTicketHolder.setNumFinishedProcessing(1);
    _tick();

    // Stable. Probe down since concurrency is already at its maximum allowed value, even though
    // ticktes are exhausted.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), size);
    ASSERT_LT(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingMinConcurrencyTest, NoProbeDown) {
    // Tickets are not exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setUsed(size - 1);
    _readTicketHolder.setNumFinishedProcessing(1);
    _tick();

    // Stable. Do not probe in either direction since tickets are not exhausted but concurrency is
    // already at its minimum allowed value.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingMinConcurrencyTest, StepSizeNonZero) {
    gStepMultiple.store(0.1);
    auto size = _readTicketHolder.outof();

    // The concurrency level is low enough that the step multiple on its own is not enough to get to
    // the next integer.
    ASSERT_EQ(std::lround(size * (1 + gStepMultiple.load())), size);

    // Tickets are exhausted.
    _readTicketHolder.setUsed(size);
    _readTicketHolder.setUsed(size - 1);
    _readTicketHolder.setNumFinishedProcessing(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size + 1);
    ASSERT_EQ(_writeTicketHolder.outof(), size + 1);

    // Throughput inreases.
    _readTicketHolder.setNumFinishedProcessing(3);
    _tick();

    // Probing up succeeds; the new value is promoted to stable.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size + 1);
    ASSERT_EQ(_writeTicketHolder.outof(), size + 1);
}

}  // namespace
}  // namespace mongo::execution_control
