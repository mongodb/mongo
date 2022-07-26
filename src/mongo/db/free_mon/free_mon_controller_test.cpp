/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>
#include <future>
#include <iostream>
#include <memory>
#include <snappy.h>

#include "mongo/db/free_mon/free_mon_controller.h"
#include "mongo/db/free_mon/free_mon_storage.h"

#include "mongo/base/data_type_validated.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/free_mon/free_mon_op_observer.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/object_check.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/hex.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

auto makeRandom() {
    auto seed = SecureRandom().nextInt64();
    LOGV2(24189, "PseudoRandom()", "seed"_attr = seed);
    return PseudoRandom(seed);
}

class FreeMonMetricsCollectorMock : public FreeMonCollectorInterface {
public:
    ~FreeMonMetricsCollectorMock() {
        // ASSERT_TRUE(_state == State::kStarted);
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) final {
        _state = State::kStarted;

        builder.append("mock", "some data");

        {
            stdx::lock_guard<Latch> lck(_mutex);

            ++_counter;

            if (_counter == _wait) {
                _condvar.notify_all();
            }
        }
    }

    std::string name() const final {
        return "mock";
    }

    void setSignalOnCount(int c) {
        _wait = c;
    }

    std::uint32_t count() {
        stdx::lock_guard<Latch> lck(_mutex);
        return _counter;
    }

    void wait() {
        stdx::unique_lock<Latch> lck(_mutex);
        while (_counter < _wait) {
            _condvar.wait(lck);
        }
    }

private:
    /**
     * Private enum to ensure caller uses class correctly.
     */
    enum class State {
        kNotStarted,
        kStarted,
    };

    // state
    State _state{State::kNotStarted};

    std::uint32_t _counter{0};

    Mutex _mutex = MONGO_MAKE_LATCH("FreeMonMetricsCollectorMock::_mutex");
    stdx::condition_variable _condvar;
    std::uint32_t _wait{0};
};

BSONArray decompressMetrics(ConstDataRange cdr) {
    std::string outBuffer;
    snappy::Uncompress(cdr.data(), cdr.length(), &outBuffer);

    ConstDataRange raw(outBuffer.data(), outBuffer.data() + outBuffer.size());
    auto swObj = raw.readNoThrow<Validated<BSONObj>>();
    ASSERT_OK(swObj.getStatus());

    return BSONArray(swObj.getValue().val["data"].Obj().getOwned());
}

/**
 * Countdown latch that propagates a message.
 */
template <typename T>
class CountdownLatchResult {
public:
    CountdownLatchResult(uint32_t count) : _count(count) {}

    /**
     * Set the count of events to wait for.
     */
    void reset(uint32_t count) {
        stdx::lock_guard<Latch> lock(_mutex);
        ASSERT_EQ(_count, 0UL);
        ASSERT_GT(count, 0UL);

        _count = count;
        _payload = T();
    }

    /**
     * Set the payload and signal waiter.
     */
    void set(T payload) {
        stdx::lock_guard<Latch> lock(_mutex);

        if (_count > 0) {
            --_count;
            if (_count == 0) {
                _payload = std::move(payload);
                _condvar.notify_one();
            }
        }
    }

    /**
     * Waits for duration until N events have occured.
     *
     * Returns boost::none on timeout.
     */
    boost::optional<T> wait_for(Milliseconds duration) {
        stdx::unique_lock<Latch> lock(_mutex);

        if (!_condvar.wait_for(
                lock, duration.toSystemDuration(), [this]() { return _count == 0; })) {
            return {};
        }

        return _payload;
    }

private:
    // Condition variable to signal consumer
    stdx::condition_variable _condvar;

    // Lock for condition variable and to protect state
    Mutex _mutex = MONGO_MAKE_LATCH("CountdownLatchResult::_mutex");

    // Count to wait fore
    uint32_t _count;

    // Provided payload
    T _payload;
};

class FreeMonNetworkInterfaceMock final : public FreeMonNetworkInterface {
public:
    struct Options {
        // If sync = true, then execute the callback immediately and the subsequent future chain
        // This allows us to ensure the follow up functions to a network request are executed
        // before anything else is processed by FreeMonProcessor
        bool doSync{false};

        // Faults to inject for registration
        bool failRegisterHttp{false};
        bool invalidRegister{false};
        bool haltRegister{false};

        // Faults to inject for metrics
        bool haltMetrics{false};
        bool fail2MetricsUploads{false};
        bool permanentlyDeleteAfter3{false};

        bool resendRegistrationAfter3{false};
    };

    explicit FreeMonNetworkInterfaceMock(executor::ThreadPoolTaskExecutor* threadPool,
                                         Options options)
        : _threadPool(threadPool), _options(options), _countdownMetrics(0) {}

    Future<FreeMonRegistrationResponse> sendRegistrationAsync(
        const FreeMonRegistrationRequest& req) final {
        LOGV2(20611, "Sending Registration ...");

        _registers.addAndFetch(1);

        auto pf = makePromiseFuture<FreeMonRegistrationResponse>();
        if (_options.doSync) {
            pf.promise.setFrom(doRegister(req));
        } else {
            auto swSchedule = _threadPool->scheduleWork(
                [sharedPromise = std::move(pf.promise), req, this](
                    const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
                    sharedPromise.setWith([&] { return doRegister(req); });
                });

            ASSERT_OK(swSchedule.getStatus());
        }

        return std::move(pf.future);
    }

    StatusWith<FreeMonRegistrationResponse> doRegister(const FreeMonRegistrationRequest& req) {

        if (_options.failRegisterHttp) {
            return Status(ErrorCodes::FreeMonHttpTemporaryFailure, "Mock failure");
        }

        auto resp = FreeMonRegistrationResponse();
        resp.setVersion(1);

        if (_options.invalidRegister) {
            resp.setVersion(42);
        }

        resp.setId("regId123");

        if (_options.haltRegister) {
            resp.setHaltMetricsUploading(true);
        }

        resp.setReportingInterval(1);

        return resp;
    }


    Future<FreeMonMetricsResponse> sendMetricsAsync(const FreeMonMetricsRequest& req) final {
        LOGV2(20612, "Sending Metrics ...");

        _metrics.addAndFetch(1);

        auto pf = makePromiseFuture<FreeMonMetricsResponse>();
        if (_options.doSync) {
            pf.promise.setFrom(doMetrics(req));
        } else {
            auto swSchedule = _threadPool->scheduleWork(
                [sharedPromise = std::move(pf.promise), req, this](
                    const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
                    sharedPromise.setWith([&] { return doMetrics(req); });
                });

            ASSERT_OK(swSchedule.getStatus());
        }

        return std::move(pf.future);
    }

    StatusWith<FreeMonMetricsResponse> doMetrics(const FreeMonMetricsRequest& req) {
        auto cdr = req.getMetrics();

        {
            stdx::lock_guard<Latch> lock(_metricsLock);
            auto metrics = decompressMetrics(cdr);
            _lastMetrics = metrics;
            _countdownMetrics.set(metrics);
        }

        if (_options.fail2MetricsUploads && _metrics.loadRelaxed() < 3) {
            return Status(ErrorCodes::FreeMonHttpTemporaryFailure, "Mock failure");
        }

        auto resp = FreeMonMetricsResponse();
        resp.setVersion(1);
        resp.setReportingInterval(1);

        resp.setId("metricsId456"_sd);

        if (_options.haltMetrics) {
            resp.setHaltMetricsUploading(true);
        }

        if (_options.permanentlyDeleteAfter3 && _metrics.loadRelaxed() > 3) {
            resp.setPermanentlyDelete(true);
        }

        if (_options.resendRegistrationAfter3 && _metrics.loadRelaxed() == 3) {
            resp.setResendRegistration(true);
        }

        return resp;
    }

    int32_t getRegistersCalls() const {
        return _registers.load();
    }

    int32_t getMetricsCalls() const {
        return _metrics.load();
    }

    boost::optional<BSONArray> waitMetricsCalls(uint32_t count, Milliseconds wait) {
        _countdownMetrics.reset(count);
        return _countdownMetrics.wait_for(wait);
    }

    BSONArray getLastMetrics() {
        stdx::lock_guard<Latch> lock(_metricsLock);
        return _lastMetrics;
    }


private:
    AtomicWord<int> _registers;
    AtomicWord<int> _metrics;

    executor::ThreadPoolTaskExecutor* _threadPool;

    Mutex _metricsLock = MONGO_MAKE_LATCH("FreeMonNetworkInterfaceMock::_metricsLock");
    BSONArray _lastMetrics;

    Options _options;

    CountdownLatchResult<BSONArray> _countdownMetrics;
};

class FreeMonControllerTest : public ServiceContextMongoDTest {

protected:
    void setUp() override;
    void tearDown() override;

protected:
    /**
     * Looks up the current ReplicationCoordinator.
     * The result is cast to a ReplicationCoordinatorMock to provide access to test features.
     */
    repl::ReplicationCoordinatorMock* _getReplCoord() const;

    ServiceContext::UniqueOperationContext _opCtx;

    executor::NetworkInterfaceMock* _mockNetwork{nullptr};

    std::unique_ptr<executor::ThreadPoolTaskExecutor> _mockThreadPool;
};

void FreeMonControllerTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    // Set up a NetworkInterfaceMock. Note, unlike NetworkInterfaceASIO, which has its own pool of
    // threads, tasks in the NetworkInterfaceMock must be carried out synchronously by the (single)
    // thread the unit test is running on.
    auto netForFixedTaskExecutor = std::make_unique<executor::NetworkInterfaceMock>();
    _mockNetwork = netForFixedTaskExecutor.get();

    // Set up a ThreadPoolTaskExecutor. Note, for local tasks this TaskExecutor uses a
    // ThreadPoolMock, and for remote tasks it uses the NetworkInterfaceMock created above. However,
    // note that the ThreadPoolMock uses the NetworkInterfaceMock's threads to run tasks, which is
    // again just the (single) thread the unit test is running on. Therefore, all tasks, local and
    // remote, must be carried out synchronously by the test thread.
    _mockThreadPool = makeThreadPoolTestExecutor(std::move(netForFixedTaskExecutor));

    _mockThreadPool->startup();

    _opCtx = cc().makeOperationContext();

    //_storage = std::make_unique<repl::StorageInterfaceImpl>();
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

    // Transition to PRIMARY so that the server can accept writes.
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));

    repl::createOplog(_opCtx.get());

    // Create collection with one document.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();

    auto statusCC = repl::StorageInterface::get(service)->createCollection(
        _opCtx.get(), NamespaceString("admin", "system.version"), collectionOptions);
    ASSERT_OK(statusCC);
}

void FreeMonControllerTest::tearDown() {
    _opCtx = {};
    ServiceContextMongoDTest::tearDown();
}

repl::ReplicationCoordinatorMock* FreeMonControllerTest::_getReplCoord() const {
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
    ASSERT(replCoord) << "No ReplicationCoordinator installed";
    auto replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock) << "Unexpected type for installed ReplicationCoordinator";
    return replCoordMock;
}

#define ASSERT_RANGE(target, lower, upper)    \
    {                                         \
        auto __x = counter.getNextDuration(); \
        ASSERT_GTE(__x, target + lower);      \
        ASSERT_LTE(__x, target + upper);      \
    }


// Positive: Ensure deadlines sort properly
TEST(FreeMonRetryTest, TestRegistration) {
    auto random = makeRandom();
    RegistrationRetryCounter counter(random);
    counter.reset();

    ASSERT_EQ(counter.getNextDuration(), Seconds(1));
    ASSERT_EQ(counter.getNextDuration(), Seconds(1));

    for (int j = 0; j < 3; j++) {
        // Fail requests
        for (int i = 1; i <= 10; ++i) {
            ASSERT_TRUE(counter.incrementError());

            int64_t base = pow(2, i);
            ASSERT_RANGE(Seconds(base), Seconds(2), Seconds(10));
        }

        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(1024), Seconds(60), Seconds(120));
        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(1024), Seconds(60), Seconds(120));

        counter.reset();
    }

    // Validate max timeout

    auto characterizeJitter = [](Seconds jitter1, Seconds jitter2) {
        static constexpr size_t kStage1Retries = 10;
        static constexpr auto kTMax = Days{2};
        auto t = Seconds(0);
        auto base = Seconds(1);
        size_t i = 0;
        for (; t < kTMax; ++i) {
            if (i < kStage1Retries) {
                base *= 2;
                t += base + jitter1;
            } else {
                t += base + jitter2;
            }
        }
        return i;
    };
    // If jitter is small as possible, we'd expect trueMax increments before false.
    const auto trueMax = characterizeJitter(Seconds{2}, Seconds{60});
    // If jitter is large as possible, we'd expect trueMin increments before false.
    const auto trueMin = characterizeJitter(Seconds{9}, Seconds{119});

    // LOGV2(20613, "trueMin:{trueMin}", "trueMin"_attr = trueMin);
    // LOGV2(20614, "trueMax:{trueMax}", "trueMax"_attr = trueMax);

    for (int j = 0; j < 30; j++) {
        // std::cout << "j: " << j << "\n";
        // Fail requests
        size_t trueCount = 0;
        while (counter.incrementError()) {
            ++trueCount;
        }
        ASSERT_GTE(trueCount, trueMin);
        ASSERT_LTE(trueCount, trueMax);
        counter.reset();
    }
}

// Positive: Ensure deadlines sort properly
TEST(FreeMonRetryTest, TestMetrics) {
    auto random = makeRandom();
    MetricsRetryCounter counter(random);
    counter.reset();

    ASSERT_EQ(counter.getNextDuration(), Seconds(1));
    ASSERT_EQ(counter.getNextDuration(), Seconds(1));

    int32_t minTime = 1;
    for (int j = 0; j < 3; j++) {
        // Fail requests
        for (int i = 0; i <= 6; ++i) {
            ASSERT_TRUE(counter.incrementError());

            int64_t base = pow(2, i);
            ASSERT_RANGE(Seconds(base), Seconds(minTime / 2), Seconds(minTime));
        }

        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(64), Seconds(minTime / 2), Seconds(minTime));
        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(64), Seconds(minTime / 2), Seconds(minTime));

        counter.reset();
    }

    // Validate max timeout
    static size_t expectation = [] {
        // There's technically a jitter in the MetricsRetryCounter but its default
        // magnitude rounds to 0, so we make an exact expectation.
        size_t iters = 0;
        static constexpr auto kDurationMax = Days{7};
        auto t = Seconds{0};
        auto base = Seconds{1};
        for (; t < kDurationMax; ++iters) {
            if (iters < 6)
                base *= 2;
            t += base;
        }
        return iters;
    }();

    for (int j = 0; j < 30; j++) {
        // Fail requests
        int iters = 0;
        while (counter.incrementError()) {
            ++iters;
        }
        ASSERT_EQ(iters, expectation);
        counter.reset();
    }
}

// Positive: Ensure the response is validated correctly
TEST(FreeMonProcessorTest, TestRegistrationResponseValidation) {
    ASSERT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // max reporting interval
    ASSERT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 30 * 60 * 60 * 24LL))));

    // Positive: version 2
    ASSERT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 2LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Positive: empty registration id string
    ASSERT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << ""
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Negative: bad protocol version
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 42LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Negative: halt uploading
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << true << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Negative: large registartation id
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id" << std::string(5000, 'a')
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Negative: large URL
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL" << std::string(5000, 'b') << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Negative: large message
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message" << std::string(5000, 'c') << "reportingInterval" << 1LL))));

    // Negative: too small a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 0LL))));

    // Negative: too large a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << (60LL * 60 * 24 * 30 + 1LL)))));
}


// Positive: Ensure the response is validated correctly
TEST(FreeMonProcessorTest, TestMetricsResponseValidation) {
    ASSERT_OK(FreeMonProcessor::validateMetricsResponse(
        FreeMonMetricsResponse::parse(IDLParserContext("foo"),

                                      BSON("version" << 1LL << "haltMetricsUploading" << false
                                                     << "permanentlyDelete" << false << "id"
                                                     << "mock123"
                                                     << "informationalURL"
                                                     << "http://www.example.com/123"
                                                     << "message"
                                                     << "msg456"
                                                     << "reportingInterval" << 1LL))));

    // Positive: Support version 2
    ASSERT_OK(FreeMonProcessor::validateMetricsResponse(
        FreeMonMetricsResponse::parse(IDLParserContext("foo"),

                                      BSON("version" << 2LL << "haltMetricsUploading" << false
                                                     << "permanentlyDelete" << false << "id"
                                                     << "mock123"
                                                     << "informationalURL"
                                                     << "http://www.example.com/123"
                                                     << "message"
                                                     << "msg456"
                                                     << "reportingInterval" << 1LL))));

    // Positive: Add resendRegistration
    ASSERT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserContext("foo"),

        BSON("version" << 2LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL << "resendRegistration" << true))));


    // Positive: max reporting interval
    ASSERT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserContext("foo"),

        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 60 * 60 * 24 * 30LL))));

    // Negative: bad protocol version
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(
        FreeMonMetricsResponse::parse(IDLParserContext("foo"),
                                      BSON("version" << 42LL << "haltMetricsUploading" << false
                                                     << "permanentlyDelete" << false << "id"
                                                     << "mock123"
                                                     << "informationalURL"
                                                     << "http://www.example.com/123"
                                                     << "message"
                                                     << "msg456"
                                                     << "reportingInterval" << 1LL))));

    // Negative: halt uploading
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(
        FreeMonMetricsResponse::parse(IDLParserContext("foo"),
                                      BSON("version" << 1LL << "haltMetricsUploading" << true
                                                     << "permanentlyDelete" << false << "id"
                                                     << "mock123"
                                                     << "informationalURL"
                                                     << "http://www.example.com/123"
                                                     << "message"
                                                     << "msg456"
                                                     << "reportingInterval" << 1LL))));

    // Negative: large registartation id
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id" << std::string(5000, 'a') << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Negative: large URL
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false

                       << "permanentlyDelete" << false << "id"
                       << "mock123"
                       << "informationalURL" << std::string(5000, 'b') << "message"
                       << "msg456"
                       << "reportingInterval" << 1LL))));

    // Negative: large message
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message" << std::string(5000, 'c') << "reportingInterval" << 1LL))));

    // Negative: too small a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(
        FreeMonMetricsResponse::parse(IDLParserContext("foo"),
                                      BSON("version" << 1LL << "haltMetricsUploading" << false
                                                     << "permanentlyDelete" << false << "id"
                                                     << "mock123"
                                                     << "informationalURL"
                                                     << "http://www.example.com/123"
                                                     << "message"
                                                     << "msg456"
                                                     << "reportingInterval" << 0LL))));

    // Negative: too large a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval" << (60LL * 60 * 24 * 30 + 1LL)))));
}

/**
 * Fluent class that encapsulates how many turns of a crank is needed to do a particular operation.
 *
 * All commands take 1 turn except registerCommand and metricsSend since these have a HTTP send an
 * HTTP receive.
 */
class Turner {
public:
    Turner() = default;

    Turner& registerServer() {
        return inc(1, 1);
    }

    Turner& registerCommand(size_t count = 1) {
        return inc(2, count);
    }

    Turner& unRegisterCommand() {
        return inc(1, 1);
    }

    Turner& collect(size_t count = 1) {
        return inc(1, count);
    }

    Turner& metricsSend(size_t count = 1) {
        return inc(2, count);
    }

    Turner& onTransitionToPrimary() {
        return inc(1, 1);
    }

    Turner& notifyUpsert() {
        return inc(1, 1);
    }

    Turner& notifyDelete() {
        return inc(1, 1);
    }

    Turner& notifyOnRollback() {
        return inc(1, 1);
    }

    operator size_t() {
        return _count;
    }

private:
    Turner& inc(size_t perOperatioCost, size_t numberOfOperations) {
        _count += (perOperatioCost * numberOfOperations);
        return *this;
    }

private:
    size_t _count;
};

/**
 * Utility class to manage controller setup and lifecycle for testing.
 */
struct ControllerHolder {
    ControllerHolder(executor::ThreadPoolTaskExecutor* pool,
                     FreeMonNetworkInterfaceMock::Options opts,
                     bool useCrankForTest = true) {
        auto registerCollectorUnique = std::make_unique<FreeMonMetricsCollectorMock>();
        auto metricsCollectorUnique = std::make_unique<FreeMonMetricsCollectorMock>();

        // If we want to manually turn the crank the queue, we must process the messages
        // synchronously
        if (useCrankForTest) {
            opts.doSync = true;
        }

        ASSERT_EQ(opts.doSync, useCrankForTest);

        auto networkUnique =
            std::unique_ptr<FreeMonNetworkInterface>(new FreeMonNetworkInterfaceMock(pool, opts));
        network = static_cast<FreeMonNetworkInterfaceMock*>(networkUnique.get());
        controller = std::make_unique<FreeMonController>(std::move(networkUnique), useCrankForTest);

        registerCollector = registerCollectorUnique.get();
        metricsCollector = metricsCollectorUnique.get();

        controller->addRegistrationCollector(std::move(registerCollectorUnique));
        controller->addMetricsCollector(std::move(metricsCollectorUnique));
    }

    ~ControllerHolder() {
        controller->stop();
    }

    void start(RegistrationType registrationType) {
        std::vector<std::string> tags;
        controller->start(registrationType, tags, Seconds(1));
    }


    FreeMonController* operator->() {
        return controller.get();
    }

    FreeMonMetricsCollectorMock* registerCollector;
    FreeMonMetricsCollectorMock* metricsCollector;
    FreeMonNetworkInterfaceMock* network;

    std::unique_ptr<FreeMonController> controller;
};

// Positive: Test Register works
TEST_F(FreeMonControllerTest, TestRegister) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::DoNotRegister);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerCommand());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 0UL);
}

// Negatve: Test Register times out if network stack drops messages
TEST_F(FreeMonControllerTest, TestRegisterTimeout) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.failRegisterHttp = true;

    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::DoNotRegister);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);
    controller->turnCrankForTest(Turner().registerCommand(2));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::pending);
    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.registerCollector->count(), 2UL);
}

// Negatve: Test Register fails if the registration is wrong
TEST_F(FreeMonControllerTest, TestRegisterFail) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.invalidRegister = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::DoNotRegister);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);
    controller->turnCrankForTest(Turner().registerCommand(1));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);
    ASSERT_EQ(controller.network->getRegistersCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
}

// Positive: Ensure registration halts
TEST_F(FreeMonControllerTest, TestRegisterHalts) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.haltRegister = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::DoNotRegister);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);
    controller->turnCrankForTest(Turner().registerCommand());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);
    ASSERT_EQ(controller.network->getRegistersCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
}

// Positive: Test Metrics works on server register
TEST_F(FreeMonControllerTest, TestMetrics) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterOnStart);

    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().collect(2).metricsSend());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 1UL);
}


// Positive: Test Metrics is collected but no registration happens on empty storage
TEST_F(FreeMonControllerTest, TestMetricsWithEmptyStorage) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().collect(4));

    ASSERT_GTE(controller.network->getRegistersCalls(), 0);
    ASSERT_GTE(controller.network->getMetricsCalls(), 0);

    ASSERT_EQ(controller.registerCollector->count(), 0UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL);
}

FreeMonStorageState initStorage(StorageStateEnum e) {
    FreeMonStorageState storage;
    storage.setVersion(1UL);

    storage.setRegistrationId("Foo");
    storage.setState(e);
    storage.setInformationalURL("http://www.example.com");
    storage.setMessage("Hello World");
    storage.setUserReminder("");
    return storage;
}

// Positive: Test Metrics is collected and implicit registration happens when storage is initialized
TEST_F(FreeMonControllerTest, TestMetricsWithEnabledStorage) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().collect(2).metricsSend());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 1UL);
}

// Positive: Test Metrics is collected but no registration happens on disabled storage
TEST_F(FreeMonControllerTest, TestMetricsWithDisabledStorage) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::disabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().collect(4));

    ASSERT_GTE(controller.network->getRegistersCalls(), 0);
    ASSERT_GTE(controller.network->getMetricsCalls(), 0);

    ASSERT_EQ(controller.registerCollector->count(), 0UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL);
}


// Positive: Test Metrics is collected but no registration happens on disabled storage until user
// registers
TEST_F(FreeMonControllerTest, TestMetricsWithDisabledStorageThenRegister) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::disabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().metricsSend().collect(4));

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerCommand().metricsSend().collect(2).metricsSend());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL + 2UL);
}

// Positive: Test Metrics is collected but no registration happens, then register, then Unregister,
// and finally register again
TEST_F(FreeMonControllerTest, TestMetricsWithDisabledStorageThenRegisterAndReregister) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::disabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().metricsSend().collect(4));

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerCommand().collect(2).metricsSend());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get())->getState() == StorageStateEnum::enabled);

    optionalStatus = controller->unregisterServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().unRegisterCommand().collect(3));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get())->getState() == StorageStateEnum::disabled);

    optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerCommand().metricsSend().collect(2).metricsSend());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get())->getState() == StorageStateEnum::enabled);

    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 2UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL + 3UL + 2UL);
}

// Positive: Test DeRegister cancels a register that is in the middle of retrying
TEST_F(FreeMonControllerTest, TestMetricsUnregisterCancelsRegister) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.failRegisterHttp = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::DoNotRegister);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);
    controller->turnCrankForTest(Turner().registerCommand(2));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::pending);

    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.registerCollector->count(), 2UL);

    optionalStatus = controller->unregisterServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().unRegisterCommand());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);

    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.registerCollector->count(), 2UL);
}

// Positive: Test Metrics halts
TEST_F(FreeMonControllerTest, TestMetricsHalt) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.haltMetrics = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::RegisterOnStart);

    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().metricsSend().collect(4).metricsSend());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL);
}


// Positive: Test Metrics permanently deletes if requested
TEST_F(FreeMonControllerTest, TestMetricsPermanentlyDelete) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.permanentlyDeleteAfter3 = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::RegisterOnStart);

    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().collect(5).metricsSend(4));

    ASSERT_FALSE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 3);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 3UL);
}

// Positive: ensure registration id rotates
TEST_F(FreeMonControllerTest, TestRegistrationIdRotatesAfterRegistration) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect(2));

    // Ensure registration rotated the id
    ASSERT_EQ(FreeMonStorage::read(_opCtx.get())->getRegistrationId(), "regId123");

    controller->turnCrankForTest(Turner().metricsSend().collect());

    // Ensure metrics rotated the id
    ASSERT_EQ(FreeMonStorage::read(_opCtx.get())->getRegistrationId(), "metricsId456");

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 1UL);
}

// Positive: ensure pre-registration metrics batching occurs
// Positive: ensure we only get two metrics each time
TEST_F(FreeMonControllerTest, TestPreRegistrationMetricBatching) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().collect(4));

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerCommand().metricsSend());

    // Ensure we sent all the metrics batched before registration
    ASSERT_EQ(controller.network->getLastMetrics().nFields(), 4);

    controller->turnCrankForTest(Turner().metricsSend().collect(1));

    // Ensure we only send 2 metrics in the normal happy case
    ASSERT_EQ(controller.network->getLastMetrics().nFields(), 2);
}

// Positive: resend registration in metrics response
TEST_F(FreeMonControllerTest, TestResendRegistration) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.resendRegistrationAfter3 = true;

    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect(2));

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    controller->turnCrankForTest(
        Turner().metricsSend(3).collect(3).registerCommand().metricsSend(1));

    ASSERT_EQ(controller.registerCollector->count(), 2UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL);
}

#if 0
// Negative: Test metrics buffers on failure, and retries and ensure 2 metrics occurs after a blip
// of an error
// Note: this test operates in real-time because it needs to test multiple retries matched with
// metrics collection.
TEST_F(FreeMonControllerTest, TestMetricBatchingOnErrorRealtime) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.fail2MetricsUploads = true;
    ControllerHolder controller(_mockThreadPool.get(), opts, false);

    controller.start(RegistrationType::RegisterOnStart);

    // Ensure the second upload sends 1 samples
    ASSERT_TRUE(controller.network->waitMetricsCalls(2, Seconds(5)).is_initialized());
    ASSERT_EQ(controller.network->getLastMetrics().nFields(), 2);

    // Ensure the third upload sends 3 samples because first failed
    ASSERT_TRUE(controller.network->waitMetricsCalls(1, Seconds(5)).is_initialized());
    ASSERT_EQ(controller.network->getLastMetrics().nFields(), 4);

    // Ensure the fourth upload sends 2 samples
    ASSERT_TRUE(controller.network->waitMetricsCalls(1, Seconds(5)).is_initialized());
    ASSERT_EQ(controller.network->getLastMetrics().nFields(), 2);
}
#endif

class FreeMonControllerRSTest : public FreeMonControllerTest {
private:
    void setUp() final;
    void tearDown() final;
};

void FreeMonControllerRSTest::setUp() {
    FreeMonControllerTest::setUp();
    auto service = getServiceContext();

    // Set up an OpObserver to exercise repl integration
    auto opObserver = std::make_unique<FreeMonOpObserver>();
    auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
    opObserverRegistry->addObserver(std::move(opObserver));
}

void FreeMonControllerRSTest::tearDown() {
    FreeMonControllerTest::tearDown();
}

// Positive: Transition to primary
TEST_F(FreeMonControllerRSTest, TransitionToPrimary) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    // Now become a secondary, then primary, and see what happens when we become primary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().collect(2));

    controller->notifyOnTransitionToPrimary();

    controller->turnCrankForTest(Turner().onTransitionToPrimary().registerCommand());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 2UL);
}

// Positive: Test metrics works on secondary
TEST_F(FreeMonControllerRSTest, StartupOnSecondary) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    // Now become a secondary, then primary, and see what happens when we become primary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    // Validate the new registration id was not written
    ASSERT_EQ(FreeMonStorage::read(_opCtx.get())->getRegistrationId(), "Foo");

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 1UL);
}

// Positive: Test registration occurs on replicated insert from primary
TEST_F(FreeMonControllerRSTest, SecondaryStartOnInsert) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().collect(2));

    controller->notifyOnUpsert(initStorage(StorageStateEnum::enabled).toBSON());

    controller->turnCrankForTest(Turner().notifyUpsert().registerCommand().collect());

    ASSERT_FALSE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 2UL);
}

// Positive: Test registration occurs on replicated update from primary
TEST_F(FreeMonControllerRSTest, SecondaryStartOnUpdate) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::pending));

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().collect(2));

    controller->notifyOnUpsert(initStorage(StorageStateEnum::enabled).toBSON());

    controller->turnCrankForTest(Turner().notifyUpsert().registerCommand().collect());

    // Since there is no local write, it remains pending
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::pending);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 2UL);
}

// Positive: Test Metrics works on secondary after opObserver de-register
TEST_F(FreeMonControllerRSTest, SecondaryStopOnDeRegister) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect(1));

    ASSERT_EQ(controller.metricsCollector->count(), 1UL);

    controller->notifyOnUpsert(initStorage(StorageStateEnum::disabled).toBSON());

    controller->turnCrankForTest(Turner().notifyUpsert().collect().metricsSend());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    // Since there is no local write, it remains enabled
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::enabled);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_EQ(controller.metricsCollector->count(), 2UL);
}

// Negative: Tricky: Primary becomes secondary during registration
TEST_F(FreeMonControllerRSTest, StepdownDuringRegistration) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerServer() + 1);

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::pending);

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    // Finish registration
    controller->turnCrankForTest(1);
    controller->turnCrankForTest(Turner().metricsSend().collect(2));

    // Registration cannot write back to the local store so remain in pending
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::pending);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_EQ(controller.metricsCollector->count(), 2UL);
}

// Negative: Tricky: Primary becomes secondary during metrics send
TEST_F(FreeMonControllerRSTest, StepdownDuringMetricsSend) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    auto optionalStatus = controller->registerServerCommand(Milliseconds::min());
    ASSERT(optionalStatus);
    ASSERT_OK(*optionalStatus);

    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect());

    // Finish registration
    controller->turnCrankForTest(Turner().collect(1) + 1);

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    // Finish send
    controller->turnCrankForTest(1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_EQ(controller.metricsCollector->count(), 2UL);
}

// Positive: Test Metrics works on secondary after opObserver delete of document
TEST_F(FreeMonControllerRSTest, SecondaryStopOnDocumentDrop) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect(1));

    ASSERT_EQ(controller.metricsCollector->count(), 1UL);

    controller->notifyOnDelete();

    // There is a race condition where sometimes metrics send sneaks in
    controller->turnCrankForTest(Turner().notifyDelete().collect(3));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    // Since there is no local write, it remains enabled
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::enabled);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 2UL);
}


// Positive: Test Metrics works on secondary after opObserver delete of document between metrics
// send and metrics async complete
TEST_F(FreeMonControllerRSTest, SecondaryStopOnDocumentDropDuringCollect) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect(1));

    ASSERT_EQ(controller.metricsCollector->count(), 1UL);

    // Crank the metrics send but not the complete
    controller->turnCrankForTest(Turner().collect(1));

    controller->notifyOnDelete();

    // Move the notify delete above the async metrics complete
    controller->deprioritizeFirstMessageForTest(FreeMonMessageType::AsyncMetricsComplete);

    // There is a race condition where sometimes metrics send sneaks in
    // Crank the notifyDelete and the async metrics complete.
    controller->turnCrankForTest(Turner().notifyDelete().collect(1));

    controller->turnCrankForTest(Turner().metricsSend().collect(2));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    // Since there is no local write, it remains enabled
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::enabled);

    BSONObjBuilder builder;
    controller->getServerStatus(_opCtx.get(), &builder);
    auto obj = builder.obj();
    ASSERT_BSONOBJ_EQ(BSON("state"
                           << "undecided"),
                      obj);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_EQ(controller.metricsCollector->count(), 5UL);
}


// Negative: Test nice shutdown on bad update
TEST_F(FreeMonControllerRSTest, SecondaryStartOnBadUpdate) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().metricsSend().collect(2));

    controller->notifyOnUpsert(BSON("version" << 2LL));

    controller->turnCrankForTest(Turner().notifyUpsert());

    // Since there is no local write, it remains enabled
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::enabled);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_EQ(controller.metricsCollector->count(), 2UL);
}

// Positive: On rollback, start registration if needed
TEST_F(FreeMonControllerRSTest, SecondaryRollbackStopMetrics) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::disabled));

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().collect(2));

    ASSERT_EQ(controller.metricsCollector->count(), 2UL);

    // Simulate a rollback by writing out of band
    // Cheat a little by flipping to primary to allow the write to succeed
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));
    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    controller->notifyOnRollback();

    controller->turnCrankForTest(
        Turner().notifyOnRollback().registerCommand().metricsSend().collect(2).metricsSend());

    // Since there is no local write, it remains enabled
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::enabled);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_EQ(controller.metricsCollector->count(), 4UL);
}

// TODO: tricky - OnUpser - disable - OnDelete - make sure registration halts
// TODO: tricky - OnDelete - make sure registration halts

// TODO: Integration: Tricky - secondary as marked via command line - enableCloudFreeMOnitorig =
// false but a primary replicates a change to enable it

// TODO: test SSL???


// TODO: Positive: ensure optional fields are rotated

}  // namespace
}  // namespace mongo
