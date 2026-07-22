// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/configsvr_coordinator_service.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_coordinator_document_gen.h"
#include "mongo/db/topology/user_write_block/set_user_write_block_mode_coordinator_document_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class ConfigsvrCoordinatorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {

public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ConfigsvrCoordinatorService>(serviceContext);
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        auto registry = repl::PrimaryOnlyServiceRegistry::get(serviceContext);
        registry->registerService(std::make_unique<ShardingCoordinatorService>(
            serviceContext, std::make_unique<ShardingCoordinatorExternalStateFactoryImpl>()));
    }

    void tearDown() override {
        _service->shutdown();
        repl::PrimaryOnlyServiceMongoDTest::tearDown();
    }
};

TEST_F(ConfigsvrCoordinatorServiceTest, CoordinatorsOfSameTypeCanExist) {
    auto opCtx = cc().makeOperationContext();

    auto* service = dynamic_cast<ConfigsvrCoordinatorService*>(_service);

    std::vector<std::shared_ptr<ConfigsvrCoordinator>> instances;
    {
        // Ensure that the new coordinators we create won't actually run.
        FailPointEnableBlock fp("hangAndEndBeforeRunningConfigsvrCoordinatorInstance");

        SetClusterParameterCoordinatorDocument coordinatorDoc;
        ConfigsvrCoordinatorId cid(ConfigsvrCoordinatorTypeEnum::kSetClusterParameter);
        cid.setSubId("0"sv);
        coordinatorDoc.setConfigsvrCoordinatorMetadata({cid});
        coordinatorDoc.setParameter(BSON("a" << 1));
        coordinatorDoc.setCompatibleWithTopologyChange(true);

        SetClusterParameterCoordinatorDocument coordinatorDocSameSubId;
        coordinatorDocSameSubId.setConfigsvrCoordinatorMetadata({cid});
        coordinatorDocSameSubId.setParameter(BSON("b" << 2));
        coordinatorDocSameSubId.setCompatibleWithTopologyChange(true);

        SetClusterParameterCoordinatorDocument coordinatorDocDiffSubId;
        ConfigsvrCoordinatorId cid1(ConfigsvrCoordinatorTypeEnum::kSetClusterParameter);
        cid1.setSubId("1"sv);
        coordinatorDocDiffSubId.setConfigsvrCoordinatorMetadata({cid1});
        coordinatorDocDiffSubId.setParameter(BSON("a" << 1));
        coordinatorDocDiffSubId.setCompatibleWithTopologyChange(true);

        SetUserWriteBlockModeCoordinatorDocument coordinatorDocDiffType;
        ConfigsvrCoordinatorId cid2(ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode);
        cid2.setSubId("0"sv);
        coordinatorDocDiffType.setConfigsvrCoordinatorMetadata({cid2});
        coordinatorDocDiffType.setBlock(true);

        // Initially, all instances of coordinators are finished.
        ASSERT_TRUE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetClusterParameter));
        ASSERT_TRUE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode));

        // Trying to create a second coordinator with exact same fields will just get current
        // coordinator.
        auto coord1 = service->getOrCreateService(opCtx.get(), coordinatorDoc.toBSON());
        auto coord1_copy = service->getOrCreateService(opCtx.get(), coordinatorDoc.toBSON());
        ASSERT(coord1);
        // Note that this is pointer equality, so there is only one real instance.
        ASSERT_EQUALS(coord1, coord1_copy);

        // Now, setClusterParameter is not finished while setUserWriteBlockMode is.
        ASSERT_FALSE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetClusterParameter));
        ASSERT_TRUE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode));

        // Trying to create a second coordinator with same type and subId but different fields will
        // fail due to conflict.
        ASSERT_THROWS(service->getOrCreateService(opCtx.get(), coordinatorDocSameSubId.toBSON()),
                      AssertionException);

        // We can create a second coordinator of the same type but different subId.
        auto coord2 = service->getOrCreateService(opCtx.get(), coordinatorDocDiffSubId.toBSON());
        ASSERT(coord2);
        ASSERT_NOT_EQUALS(coord1, coord2);
        ASSERT_FALSE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetClusterParameter));
        ASSERT_TRUE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode));

        // We can create a coordinator with different type and same (or different) subId.
        auto coord3 = service->getOrCreateService(opCtx.get(), coordinatorDocDiffType.toBSON());
        ASSERT(coord3);
        ASSERT_NOT_EQUALS(coord1, coord3);
        ASSERT_NOT_EQUALS(coord2, coord3);
        ASSERT_FALSE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetClusterParameter));
        ASSERT_FALSE(service->areAllCoordinatorsOfTypeFinished(
            opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode));

        // Ensure all instances start before we disable the failpoint.
        fp->waitForTimesEntered(fp.initialTimesEntered() + 5);
        instances = {coord1, coord2, coord3};
    }

    for (const auto& instance : instances) {
        instance->getCompletionFuture().wait();
    }

    // All coordinators of both types should have finished.
    ASSERT_TRUE(service->areAllCoordinatorsOfTypeFinished(
        opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetClusterParameter));
    ASSERT_TRUE(service->areAllCoordinatorsOfTypeFinished(
        opCtx.get(), ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode));
}

// CausalityBarrier mock so tests can assert whether the barrier is performed, without contacting
// participants.
class MockCausalityBarrier : public CausalityBarrier {
public:
    MOCK_METHOD(void, perform, (OperationContext*, const OperationSessionInfo&), (override));
};

// Test-only coordinator that injects a caller-provided CausalityBarrier and can force a number of
// retriable failures to trigger re-executions. It reuses the SetClusterParameter state document and
// phase enum so no test-only IDL type is needed; the parameter contents are irrelevant.
class BarrierTestCoordinator
    : public ConfigsvrCoordinatorImpl<SetClusterParameterCoordinatorDocument,
                                      SetClusterParameterCoordinatorPhaseEnum> {
public:
    using StateDoc = SetClusterParameterCoordinatorDocument;
    using Phase = SetClusterParameterCoordinatorPhaseEnum;

    BarrierTestCoordinator(const BSONObj& stateDoc,
                           std::unique_ptr<CausalityBarrier> barrier,
                           int forcedRetries)
        : ConfigsvrCoordinatorImpl(stateDoc),
          _barrier(std::move(barrier)),
          _remainingRetries(forcedRetries) {}

    bool hasSameOptions(const BSONObj&) const override {
        return true;
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode,
        MongoProcessInterface::CurrentOpSessionsMode) noexcept override {
        return boost::none;
    }

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken&) noexcept override {
        return ExecutorFuture<void>(**executor)
            .then(_buildPhaseHandler(
                Phase::kSetClusterParameter, [this, anchor = shared_from_this()](auto* opCtx) {
                    // Acquire and durably persist a session, simulating a coordinator that has
                    // issued retryable writes to participants. This is what makes a subsequent
                    // execution's causality barrier do work.
                    _getNewSession(opCtx);

                    // Force a retriable error on the first `_remainingRetries` attempts so that the
                    // coordinator re-executes.
                    if (_remainingRetries-- > 0) {
                        uasserted(ErrorCodes::HostUnreachable,
                                  "forced retriable error to trigger re-execution");
                    }
                }));
    }

    const ConfigsvrCoordinatorMetadata& metadata() const override {
        return _doc.getConfigsvrCoordinatorMetadata();
    }

    std::string_view serializePhase(const Phase& phase) const override {
        return idl::serialize(phase);
    }

    std::unique_ptr<CausalityBarrier> _makeCausalityBarrier(
        const std::shared_ptr<executor::ScopedTaskExecutor>&, const CancellationToken&) override {
        invariant(_barrier, "BarrierTestCoordinator barrier already consumed");
        return std::move(_barrier);
    }

    std::unique_ptr<CausalityBarrier> _barrier;
    // Only mutated from the (single) coordinator executor thread across re-executions.
    int _remainingRetries;
};

// Minimal PrimaryOnlyService that builds BarrierTestCoordinator instances, injecting the barrier
// and forced-retry count staged on the fixture.
class BarrierTestService : public repl::PrimaryOnlyService {
public:
    BarrierTestService(ServiceContext* serviceContext,
                       std::unique_ptr<CausalityBarrier>* nextBarrier,
                       const int* forcedRetries)
        : PrimaryOnlyService(serviceContext),
          _nextBarrier(nextBarrier),
          _forcedRetries(forcedRetries) {}

    std::string_view getServiceName() const override {
        return "BarrierTestConfigsvrCoordinatorService"sv;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kConfigsvrCoordinatorsNamespace;
    }

    void checkIfConflictsWithOtherInstances(
        OperationContext*,
        BSONObj,
        const std::vector<const PrimaryOnlyService::Instance*>&) override {}

    std::shared_ptr<Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<BarrierTestCoordinator>(
            std::move(initialState), std::move(*_nextBarrier), *_forcedRetries);
    }

    std::shared_ptr<ConfigsvrCoordinator> getOrCreate(OperationContext* opCtx, BSONObj coorDoc) {
        auto [instance, _] = PrimaryOnlyService::getOrCreateInstance(opCtx, std::move(coorDoc));
        return checked_pointer_cast<ConfigsvrCoordinator>(std::move(instance));
    }

private:
    std::unique_ptr<CausalityBarrier>* _nextBarrier;
    const int* _forcedRetries;
};

class ConfigsvrCoordinatorBarrierTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<BarrierTestService>(serviceContext, &_nextBarrier, &_forcedRetries);
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        repl::StorageInterface::set(serviceContext, std::make_unique<repl::StorageInterfaceMock>());
    }

    void tearDown() override {
        _service->shutdown();
        repl::PrimaryOnlyServiceMongoDTest::tearDown();
    }

protected:
    // Runs a coordinator through its real run() loop to completion, injecting `barrier` and forcing
    // `forcedRetries` retriable failures so that the coordinator re-executes that many times.
    void runCoordinatorToCompletion(std::unique_ptr<CausalityBarrier> barrier, int forcedRetries) {
        _nextBarrier = std::move(barrier);
        _forcedRetries = forcedRetries;

        auto opCtx = cc().makeOperationContext();
        auto* service = checked_cast<BarrierTestService*>(_service);

        SetClusterParameterCoordinatorDocument doc;
        ConfigsvrCoordinatorId cid(ConfigsvrCoordinatorTypeEnum::kSetClusterParameter);
        cid.setSubId("barrier-test"sv);
        doc.setConfigsvrCoordinatorMetadata({cid});
        doc.setParameter(BSON("a" << 1));
        doc.setCompatibleWithTopologyChange(true);

        auto instance = service->getOrCreate(opCtx.get(), doc.toBSON());
        instance->getCompletionFuture().get();
    }

    // Staged for the next coordinator that the service constructs.
    std::unique_ptr<CausalityBarrier> _nextBarrier;
    int _forcedRetries = 0;
};

TEST_F(ConfigsvrCoordinatorBarrierTest, NoBarrierOnFirstRun) {
    // A coordinator that completes on its first execution never persisted a session before the
    // barrier ran, so the barrier is never performed.
    auto barrier = std::make_unique<MockCausalityBarrier>();
    EXPECT_CALL(*barrier, perform(::testing::_, ::testing::_)).Times(0);

    runCoordinatorToCompletion(std::move(barrier), 0 /* forcedRetries */);
}

TEST_F(ConfigsvrCoordinatorBarrierTest, BarrierOnReExecution) {
    // The first execution persists a session and then fails, forcing a re-execution. The
    // re-execution performs the causality barrier exactly once before doing any work.
    auto barrier = std::make_unique<MockCausalityBarrier>();
    EXPECT_CALL(*barrier, perform(::testing::_, ::testing::_)).Times(1);

    runCoordinatorToCompletion(std::move(barrier), 1 /* forcedRetries */);
}

}  // namespace
}  // namespace mongo
