/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC


#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>
#include <future>
#include <iostream>

#include "mongo/db/free_mon/free_mon_controller.h"
#include "mongo/db/free_mon/free_mon_storage.h"

#include "mongo/base/data_type_validated.h"
#include "mongo/base/deinitializer_context.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"


namespace mongo {
namespace {

class FreeMonNetworkInterfaceMock : public FreeMonNetworkInterface {
public:
    struct Options {
        bool failRegisterHttp{false};
        bool invalidRegister{false};
    };

    explicit FreeMonNetworkInterfaceMock(executor::ThreadPoolTaskExecutor* threadPool,
                                         Options options)
        : _threadPool(threadPool), _options(options) {}
    ~FreeMonNetworkInterfaceMock() final = default;

    Future<FreeMonRegistrationResponse> sendRegistrationAsync(
        const FreeMonRegistrationRequest& req) final {
        log() << "Sending Registration ...";

        _registers.addAndFetch(1);

        Promise<FreeMonRegistrationResponse> promise;
        auto future = promise.getFuture();
        auto shared_promise = promise.share();

        auto swSchedule = _threadPool->scheduleWork([shared_promise, req, this](
            const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {

            if (_options.failRegisterHttp) {
                shared_promise.setError(
                    Status(ErrorCodes::FreeMonHttpTemporaryFailure, "Mock failure"));
                return;
            }

            auto resp = FreeMonRegistrationResponse();
            resp.setVersion(1);

            if (_options.invalidRegister) {
                resp.setVersion(42);
            }

            if (req.getId().is_initialized()) {
                resp.setId(req.getId().get());
            } else {
                resp.setId(UUID::gen().toString());
            }

            resp.setReportingInterval(1);

            shared_promise.emplaceValue(resp);
        });
        ASSERT_OK(swSchedule.getStatus());

        return future;
    }

    Future<FreeMonMetricsResponse> sendMetricsAsync(const FreeMonMetricsRequest& req) final {
        log() << "Sending Metrics ...";
        ASSERT_FALSE(req.getId().empty());

        _metrics.addAndFetch(1);

        Promise<FreeMonMetricsResponse> promise;
        auto future = promise.getFuture();
        auto shared_promise = promise.share();

        auto swSchedule = _threadPool->scheduleWork(
            [shared_promise, req](const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
                auto resp = FreeMonMetricsResponse();
                resp.setVersion(1);
                resp.setReportingInterval(1);

                shared_promise.emplaceValue(resp);
            });
        ASSERT_OK(swSchedule.getStatus());


        return future;
    }

    int32_t getRegistersCalls() const {
        return _registers.load();
    }
    int32_t getMetricsCalls() const {
        return _metrics.load();
    }

private:
    AtomicInt32 _registers;
    AtomicInt32 _metrics;

    executor::ThreadPoolTaskExecutor* _threadPool;

    Options _options;
};

class FreeMonControllerTest : public ServiceContextMongoDTest {

private:
    void setUp() final;
    void tearDown() final;

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

    //_storage = stdx::make_unique<repl::StorageInterfaceImpl>();
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());


    // Transition to PRIMARY so that the server can accept writes.
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));


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
    PseudoRandom random(0);
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
}

// Positive: Ensure the response is validated correctly
TEST(AFreeMonProcessorTest, TestResponseValidation) {
    ASSERT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: bad protocol version
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 42LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: halt uploading
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << true << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large registartation id
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id" << std::string(5000, 'a')
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large URL
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << std::string(5000, 'b')
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large message
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << std::string(5000, 'c')
                       << "reportingInterval"
                       << 1LL))));

    // Negative: too small a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 0LL))));

    // Negative: too large a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << (60LL * 60 * 24 + 1LL)))));
}

// Positive: Test Register works
TEST_F(FreeMonControllerTest, TestRegister) {
    // FreeMonNetworkInterfaceMock network;
    FreeMonController controller(
        std::unique_ptr<FreeMonNetworkInterface>(new FreeMonNetworkInterfaceMock(
            _mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options())));

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_OK(controller.registerServerCommand(duration_cast<Milliseconds>(Seconds(5))));

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    controller.stop();
}

// Negatve: Test Register times out if network stack drops messages
TEST_F(FreeMonControllerTest, TestRegisterTimeout) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.failRegisterHttp = true;
    auto networkUnique = std::unique_ptr<FreeMonNetworkInterface>(
        new FreeMonNetworkInterfaceMock(_mockThreadPool.get(), opts));
    auto network = static_cast<FreeMonNetworkInterfaceMock*>(networkUnique.get());
    FreeMonController controller(std::move(networkUnique));

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_NOT_OK(controller.registerServerCommand(duration_cast<Milliseconds>(Seconds(15))));

    ASSERT_FALSE(FreeMonStorage::read(_opCtx.get()).is_initialized());
    ASSERT_GTE(network->getRegistersCalls(), 2);

    controller.stop();
}

// Negatve: Test Register times out if the registration is wrong
TEST_F(FreeMonControllerTest, TestRegisterFail) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.invalidRegister = true;
    auto networkUnique = std::unique_ptr<FreeMonNetworkInterface>(
        new FreeMonNetworkInterfaceMock(_mockThreadPool.get(), opts));
    auto network = static_cast<FreeMonNetworkInterfaceMock*>(networkUnique.get());
    FreeMonController controller(std::move(networkUnique));

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_NOT_OK(controller.registerServerCommand(duration_cast<Milliseconds>(Seconds(15))));

    ASSERT_FALSE(FreeMonStorage::read(_opCtx.get()).is_initialized());
    ASSERT_EQ(network->getRegistersCalls(), 1);

    controller.stop();
}

}  // namespace
}  // namespace mongo
