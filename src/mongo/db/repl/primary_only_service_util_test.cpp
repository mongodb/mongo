// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/primary_only_service_util.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"

#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

using namespace mongo;
using namespace mongo::repl;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace {
using namespace std::literals::string_view_literals;
constexpr auto kTestPrimaryOnlyServiceName = "TestService"sv;
constexpr auto kTestPrimaryOnlyServiceInstanceName = "TestServiceInstance"sv;
constexpr auto kTestPrimaryOnlyServiceStateDocumentNss = "config.test_primary_only_service"sv;

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
    NamespaceString::createNamespaceString_forTest(kTestPrimaryOnlyServiceStateDocumentNss)};

/**
 * Test class for the 'DefaultPrimaryOnlyServiceInstance'.
 */
class TestDefaultPrimaryOnlyServiceInstance : public DefaultPrimaryOnlyServiceInstance {
public:
    std::string_view getInstanceName() final {
        return kTestPrimaryOnlyServiceInstanceName;
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final {
        return boost::none;
    };

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final {};

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

    ~TestDefaultPrimaryOnlyService() override = default;

    std::string_view getServiceName() const final {
        return kTestPrimaryOnlyServiceName;
    }

    NamespaceString getStateDocumentsNS() const final {
        return NamespaceString::createNamespaceString_forTest(
            kTestPrimaryOnlyServiceStateDocumentNss);
    }

    static TestDefaultPrimaryOnlyService* getService(OperationContext* opCtx) {
        auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
        auto service = registry->lookupServiceByName(kTestPrimaryOnlyServiceInstanceName);
        return checked_cast<TestDefaultPrimaryOnlyService*>(std::move(service));
    }

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) final {};

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

    void shutdownHook() override {
        _registry->onShutdown();
        _service = nullptr;
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
