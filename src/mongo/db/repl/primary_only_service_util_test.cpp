/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/primary_only_service_util.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

using namespace mongo;
using namespace mongo::repl;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace {
constexpr auto kTestPrimaryOnlyServiceName = "TestService"_sd;
constexpr auto kTestPrimaryOnlyServiceInstanceName = "TestServiceInstance"_sd;
constexpr auto kTestPrimaryOnlyServiceStateDocumentNss = "config.test_primary_only_service"_sd;

// Hangs the 'TestDefaultPrimaryOnlyServiceInstance' after inserting the state document.
MONGO_FAIL_POINT_DEFINE(HangTestPrimaryOnlyServiceAfterStateDocInsertion);

/**
 * Represents the state document class for the 'TestDefaultPrimaryOnlyService'.
 */
class TestStateDocument {
public:
    void setState(int state) {
        _state = state;
    }

    BSONObj toBSON() const {
        return BSON("_id" << _state);
    }

private:
    int _state = 0;
};

// Create a global instance of the 'PrimaryOnlyServiceStateStore' to store the state document.
PrimaryOnlyServiceStateStore<TestStateDocument> gStateDocStore{
    NamespaceString{kTestPrimaryOnlyServiceStateDocumentNss}};

/**
 * Test class for the 'DefaultPrimaryOnlyServiceInstance'.
 */
class TestDefaultPrimaryOnlyServiceInstance : public DefaultPrimaryOnlyServiceInstance {
public:
    StringData getInstanceName() final {
        return kTestPrimaryOnlyServiceInstanceName;
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final {
        return boost::none;
    };

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final{};

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept final {

        return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            TestStateDocument testStateDoc;
            testStateDoc.setState(1);
            gStateDocStore.add(opCtx, testStateDoc);

            if (MONGO_unlikely(HangTestPrimaryOnlyServiceAfterStateDocInsertion.shouldFail())) {
                HangTestPrimaryOnlyServiceAfterStateDocInsertion.pauseWhileSet();
            }
        });
    };

    void _removeStateDocument(OperationContext* opCtx) final {
        gStateDocStore.remove(opCtx, BSON("_id" << 1));
    };
};

/**
 * Test class for the 'PrimaryOnlyService'.
 */
class TestDefaultPrimaryOnlyService final : public repl::PrimaryOnlyService {
public:
    explicit TestDefaultPrimaryOnlyService(ServiceContext* serviceContext)
        : repl::PrimaryOnlyService(serviceContext) {}

    ~TestDefaultPrimaryOnlyService() = default;

    StringData getServiceName() const final {
        return kTestPrimaryOnlyServiceName;
    }

    NamespaceString getStateDocumentsNS() const final {
        return NamespaceString(kTestPrimaryOnlyServiceStateDocumentNss);
    }

    ThreadPool::Limits getThreadPoolLimits() const final {
        return ThreadPool::Limits();
    }

    static TestDefaultPrimaryOnlyService* getService(OperationContext* opCtx) {
        auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
        auto service = registry->lookupServiceByName(kTestPrimaryOnlyServiceInstanceName);
        return checked_cast<TestDefaultPrimaryOnlyService*>(std::move(service));
    }

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) final{};

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) final {
        return std::make_shared<TestDefaultPrimaryOnlyServiceInstance>();
    }
};

}  // namespace

class DefaultPrimaryOnlyServiceInstanceTest : public PrimaryOnlyServiceMongoDTest {
public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<TestDefaultPrimaryOnlyService>(serviceContext);
    }

    void setUp() override {
        PrimaryOnlyServiceMongoDTest::setUp();

        _service = _registry->lookupServiceByName(kTestPrimaryOnlyServiceName);
        ASSERT(_service);
    }

    void tearDown() override {
        // Ensure that even on test failures all failpoint state gets reset.
        globalFailPointRegistry().disableAllFailpoints();

        WaitForMajorityService::get(getServiceContext()).shutDown();

        _registry->onShutdown();
        _service = nullptr;

        ServiceContextMongoDTest::tearDown();
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        PrimaryOnlyServiceMongoDTest::stepUp(opCtx.get());
    }
};

TEST_F(DefaultPrimaryOnlyServiceInstanceTest, VerifyTaskExecuted) {
    // Initialize the fail point to hang the 'TestDefaultPrimaryOnlyServiceInstance' at the
    // '_runImpl' method.
    auto timesEntered =
        HangTestPrimaryOnlyServiceAfterStateDocInsertion.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();

    // Create an instance of the 'TestDefaultPrimaryOnlyServiceInstance'.
    auto instance = TestDefaultPrimaryOnlyServiceInstance::getOrCreate(
        opCtx.get(), _service, BSON("_id" << 0), true);
    ASSERT(instance.get());

    // Wait until the fail point is hit.
    HangTestPrimaryOnlyServiceAfterStateDocInsertion.waitForTimesEntered(timesEntered + 1);

    // Verify that the state document has been inserted. This verifies that the method
    // 'TestDefaultPrimaryOnlyServiceInstance::_runImpl' has been executed.
    ASSERT_EQ(gStateDocStore.count(opCtx.get(), BSON("_id" << 1)), 1);
    HangTestPrimaryOnlyServiceAfterStateDocInsertion.setMode(FailPoint::off);

    // Wait for the instance to complete.
    instance->getCompletionFuture().get();

    // Verify that the state document has now been removed. This verifies that the method
    // 'DefaultPrimaryOnlyServiceInstanceTest::_removeStateDocument' has been executed.
    ASSERT_EQ(gStateDocStore.count(opCtx.get(), BSON("_id" << 1)), 0);
}
