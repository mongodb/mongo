// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/execution_admission_context.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_type_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo::admission::execution_control {
namespace {
using namespace std::literals::string_view_literals;

constexpr std::string_view kFieldName = "executionAdmissionContextType"sv;

using TaskType = ExecutionAdmissionContext::TaskType;
using ScopedTaskTypeVariant =
    std::variant<ScopedTaskTypeBackground, ScopedTaskTypeNonDeprioritizable>;

struct ScopedTaskTypeHolder {
    std::unique_ptr<ScopedTaskTypeVariant> _holder;

    void emplace(OperationContext* opCtx, TaskType type) {
        switch (type) {
            case TaskType::Background:
                _holder = std::make_unique<ScopedTaskTypeVariant>(
                    std::in_place_type<ScopedTaskTypeBackground>, opCtx);
                break;
            case TaskType::NonDeprioritizable:
                _holder = std::make_unique<ScopedTaskTypeVariant>(
                    std::in_place_type<ScopedTaskTypeNonDeprioritizable>, opCtx);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }

    void reset() {
        _holder.reset();
    }
};

class ScopedTaskTypeTest : public ServiceContextTest, public testing::WithParamInterface<TaskType> {
protected:
    TaskType taskType() const {
        return GetParam();
    }

    ExecutionAdmissionContext& getAdmCtx(OperationContext* opCtx) {
        return ExecutionAdmissionContext::get(opCtx);
    }
};

INSTANTIATE_TEST_SUITE_P(BackgroundAndNonDeprioritizable,
                         ScopedTaskTypeTest,
                         testing::Values(TaskType::Background, TaskType::NonDeprioritizable));

TEST_P(ScopedTaskTypeTest, DefaultTaskType) {
    auto opCtx = makeOperationContext();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_P(ScopedTaskTypeTest, ScopedSetsAndRestores) {
    auto opCtx = makeOperationContext();
    {
        ScopedTaskTypeHolder holder;
        holder.emplace(opCtx.get(), taskType());
        ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());
    }
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_P(ScopedTaskTypeTest, NestedDoesNotResetOnInnerDestruction) {
    auto opCtx = makeOperationContext();

    ScopedTaskTypeHolder outer;
    outer.emplace(opCtx.get(), taskType());
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    {
        ScopedTaskTypeHolder inner;
        inner.emplace(opCtx.get(), taskType());
        ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());
    }

    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    outer.reset();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_P(ScopedTaskTypeTest, TripleNested) {
    auto opCtx = makeOperationContext();

    ScopedTaskTypeHolder first, second, third;

    first.emplace(opCtx.get(), taskType());
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    second.emplace(opCtx.get(), taskType());
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    third.emplace(opCtx.get(), taskType());
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    third.reset();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    second.reset();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    first.reset();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_P(ScopedTaskTypeTest, SequentialScopes) {
    auto opCtx = makeOperationContext();

    {
        ScopedTaskTypeHolder holder;
        holder.emplace(opCtx.get(), taskType());
        ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());
    }
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);

    {
        ScopedTaskTypeHolder holder;
        holder.emplace(opCtx.get(), taskType());
        ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());
    }
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_P(ScopedTaskTypeTest, ExplicitLifetimeWithMultipleHolders) {
    auto opCtx = makeOperationContext();

    ScopedTaskTypeHolder a, b, c;

    a.emplace(opCtx.get(), taskType());
    b.emplace(opCtx.get(), taskType());
    c.emplace(opCtx.get(), taskType());

    a.reset();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    b.reset();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());

    c.reset();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_P(ScopedTaskTypeTest, TaskTypeDoesNotAffectPriority) {
    auto opCtx = makeOperationContext();
    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kNormal);

    ScopedTaskTypeHolder holder;
    holder.emplace(opCtx.get(), taskType());

    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kNormal);
}

TEST_P(ScopedTaskTypeTest, TaskTypeWorksWithExemptPriority) {
    auto opCtx = makeOperationContext();

    ScopedAdmissionPriority<ExecutionAdmissionContext> exemptPriority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kExempt);

    ScopedTaskTypeHolder holder;
    holder.emplace(opCtx.get(), taskType());

    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), taskType());
    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kExempt);
}

class TaskTypeTest : public ServiceContextTest {
protected:
    ExecutionAdmissionContext& getAdmCtx(OperationContext* opCtx) {
        return ExecutionAdmissionContext::get(opCtx);
    }
};

TEST_F(TaskTypeTest, SetTaskTypeNonDeprioritizable) {
    auto opCtx = makeOperationContext();
    ASSERT_FALSE(getAdmCtx(opCtx.get()).getMarkedNonDeprioritizable());
    getAdmCtx(opCtx.get()).setTaskType(opCtx.get(), TaskType::NonDeprioritizable);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
}

TEST_F(TaskTypeTest, MultipleCalls) {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setTaskType(opCtx.get(), TaskType::NonDeprioritizable);
    getAdmCtx(opCtx.get()).setTaskType(opCtx.get(), TaskType::NonDeprioritizable);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
}

TEST_F(TaskTypeTest, SetterPlusScoped) {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setTaskType(opCtx.get(), TaskType::NonDeprioritizable);
    ASSERT_TRUE(getAdmCtx(opCtx.get()).getMarkedNonDeprioritizable());
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
    {
        ScopedTaskTypeNonDeprioritizable nd(opCtx.get());
        ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
    }
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
}

TEST_F(TaskTypeTest, NonDeprioritizableSetsMarkedFlag) {
    auto opCtx = makeOperationContext();
    ASSERT_FALSE(getAdmCtx(opCtx.get()).getMarkedNonDeprioritizable());

    {
        ScopedTaskTypeNonDeprioritizable scope(opCtx.get());
        ASSERT_TRUE(getAdmCtx(opCtx.get()).getMarkedNonDeprioritizable());
    }

    // The flag is sticky -- persists after the scope is destroyed.
    ASSERT_TRUE(getAdmCtx(opCtx.get()).getMarkedNonDeprioritizable());
}

TEST_F(TaskTypeTest, SequentialDifferentTypes) {
    auto opCtx = makeOperationContext();

    {
        ScopedTaskTypeBackground bg(opCtx.get());
        ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Background);
    }
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);

    {
        ScopedTaskTypeNonDeprioritizable nd(opCtx.get());
        ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
    }
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

// TODO (SERVER-122847): Remove this test.
TEST_F(TaskTypeTest, WriteMetadataNoFieldWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", false);

    auto opCtx = makeOperationContext();
    ScopedAdmissionPriority<ExecutionAdmissionContext> lowPriority(
        opCtx.get(), AdmissionContext::Priority::kLow);

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    ASSERT_FALSE(builder.asTempObj().hasField(kFieldName));
}

// TODO (SERVER-122847): Remove the feature flag controller from the following tests.
TEST_F(TaskTypeTest, WriteMetadataNoFieldForNormalPriorityDefaultTaskType) {
    unittest::ServerParameterGuard featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", true);
    auto opCtx = makeOperationContext();

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    ASSERT_FALSE(builder.asTempObj().hasField(kFieldName));
}

TEST_F(TaskTypeTest, WriteMetadataLowPriority) {
    unittest::ServerParameterGuard featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", true);
    auto opCtx = makeOperationContext();
    ScopedAdmissionPriority<ExecutionAdmissionContext> lowPriority(
        opCtx.get(), AdmissionContext::Priority::kLow);

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    auto obj = builder.obj();
    ASSERT_TRUE(obj.hasField(kFieldName));
    ASSERT_EQ(obj[kFieldName].str(), "Low");
}

TEST_F(TaskTypeTest, WriteMetadataExemptPriority) {
    unittest::ServerParameterGuard featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", true);
    auto opCtx = makeOperationContext();
    ScopedAdmissionPriority<ExecutionAdmissionContext> exemptPriority(
        opCtx.get(), AdmissionContext::Priority::kExempt);

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    auto obj = builder.obj();
    ASSERT_TRUE(obj.hasField(kFieldName));
    ASSERT_EQ(obj[kFieldName].str(), "Exempt");
}

TEST_F(TaskTypeTest, WriteMetadataBackgroundTaskType) {
    unittest::ServerParameterGuard featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", true);
    auto opCtx = makeOperationContext();
    ScopedTaskTypeBackground bg(opCtx.get());

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    auto obj = builder.obj();
    ASSERT_TRUE(obj.hasField(kFieldName));
    ASSERT_EQ(obj[kFieldName].str(), "Background");
}

TEST_F(TaskTypeTest, WriteMetadataNonDeprioritizableTaskType) {
    unittest::ServerParameterGuard featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", true);
    auto opCtx = makeOperationContext();
    ScopedTaskTypeNonDeprioritizable nd(opCtx.get());

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    auto obj = builder.obj();
    ASSERT_TRUE(obj.hasField(kFieldName));
    ASSERT_EQ(obj[kFieldName].str(), "NonDeprioritizable");
}

TEST_F(TaskTypeTest, WriteMetadataExemptPriorityTakesPrecedenceOverTaskType) {
    unittest::ServerParameterGuard featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", true);
    auto opCtx = makeOperationContext();
    ScopedAdmissionPriority<ExecutionAdmissionContext> exemptPriority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
    ScopedTaskTypeBackground bg(opCtx.get());

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    auto obj = builder.obj();
    ASSERT_TRUE(obj.hasField(kFieldName));
    ASSERT_EQ(obj[kFieldName].str(), "Exempt");
}

TEST_F(TaskTypeTest, SetFromMetadataDoesNothingOnNullopt) {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setFromMetadata(opCtx.get(), boost::none);

    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kNormal);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_F(TaskTypeTest, SetFromMetadataSetsLowPriority) {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setFromMetadata(opCtx.get(), ExecutionAdmissionTypeEnum::kLow);

    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kLow);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_F(TaskTypeTest, SetFromMetadataSetsExemptPriority) {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setFromMetadata(opCtx.get(), ExecutionAdmissionTypeEnum::kExempt);

    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kExempt);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Default);
}

TEST_F(TaskTypeTest, SetFromMetadataSetsBackgroundTaskType) {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setFromMetadata(opCtx.get(), ExecutionAdmissionTypeEnum::kBackground);

    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kNormal);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::Background);
}

TEST_F(TaskTypeTest, SetFromMetadataSetsNonDeprioritizableTaskType) {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get())
        .setFromMetadata(opCtx.get(), ExecutionAdmissionTypeEnum::kNonDeprioritizable);

    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kNormal);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
}

TEST_F(TaskTypeTest, SetFromMetadataSkipsInvariantForPriorityPortClient) {
    // Simulate a priority port client using MockPrioritySession.
    transport::TransportLayerMock transportLayer;
    transportLayer.createSessionHook = [](transport::TransportLayer* tl) {
        return std::make_shared<transport::MockPrioritySession>(tl);
    };
    auto priorityClient = getServiceContext()->getService()->makeClient(
        "priorityPortClient", transportLayer.createSession());
    auto opCtx = priorityClient->makeOperationContext();

    // The ClientObserver sets NonDeprioritizable at opCtx creation for priority port clients.
    ASSERT_TRUE(opCtx->getClient()->isPriorityPortClient());
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);

    // setFromMetadata with no metadata (the only case for priority port clients, since
    // server-to-server commands never use the priority port) should not crash and should
    // preserve NonDeprioritizable.
    getAdmCtx(opCtx.get()).setFromMetadata(opCtx.get(), boost::none);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getTaskType(), TaskType::NonDeprioritizable);
    ASSERT_EQ(getAdmCtx(opCtx.get()).getPriority(), AdmissionContext::Priority::kNormal);
}

using TicketAdmissionStatsTest = TaskTypeTest;

TEST_F(TicketAdmissionStatsTest, RecorderAccumulatesAndForwardsEvents) {
    auto opCtx = makeOperationContext();
    auto& admCtx = getAdmCtx(opCtx.get());

    TicketAdmissionStats forwarded;
    int updates = 0;
    ScopedTicketAdmissionStatsRecorder recorder(
        opCtx.get(), [&](const TicketAdmissionStats& delta) {
            forwarded.timeQueuedMicros += delta.timeQueuedMicros;
            forwarded.timeProcessingMicros += delta.timeProcessingMicros;
            forwarded.admissions += delta.admissions;
            forwarded.lowPriorityAdmissions += delta.lowPriorityAdmissions;
            forwarded.startedQueueing += delta.startedQueueing;
            forwarded.finishedQueueing += delta.finishedQueueing;
            ++updates;
        });

    ASSERT_EQ(admCtx.getTicketStatsRecorder(), &recorder);

    admCtx.recordExecutionStartQueueing();

    // While queued, startedQueueing - finishedQueueing is 1.
    ASSERT_EQ(recorder.stats().startedQueueing - recorder.stats().finishedQueueing, 1);

    admCtx.recordExecutionWaitedAcquisition(Microseconds{100},
                                            ExecutionAdmissionContext::QueueType::kNormal);
    admCtx.recordExecutionAcquisition(AdmissionContext::Priority::kNormal,
                                      ExecutionAdmissionContext::QueueType::kNormal);
    admCtx.recordExecutionRelease(Microseconds{50});
    admCtx.recordExecutionStartQueueing();
    admCtx.recordExecutionWaitedAcquisition(Microseconds{30},
                                            ExecutionAdmissionContext::QueueType::kLow);
    admCtx.recordExecutionAcquisition(AdmissionContext::Priority::kLow,
                                      ExecutionAdmissionContext::QueueType::kLow);
    admCtx.recordExecutionRelease(Microseconds{20});

    ASSERT_EQ(updates, 8);
    ASSERT_EQ(recorder.stats().timeQueuedMicros, 130);
    ASSERT_EQ(recorder.stats().timeProcessingMicros, 70);
    ASSERT_EQ(recorder.stats().admissions, 2);
    ASSERT_EQ(recorder.stats().lowPriorityAdmissions, 1);
    ASSERT_EQ(recorder.stats().startedQueueing, 2);
    ASSERT_EQ(recorder.stats().finishedQueueing, 2);

    // The forwarded deltas add up to the local stats.
    ASSERT_EQ(forwarded.timeQueuedMicros, recorder.stats().timeQueuedMicros);
    ASSERT_EQ(forwarded.timeProcessingMicros, recorder.stats().timeProcessingMicros);
    ASSERT_EQ(forwarded.admissions, recorder.stats().admissions);
    ASSERT_EQ(forwarded.lowPriorityAdmissions, recorder.stats().lowPriorityAdmissions);
    ASSERT_EQ(forwarded.startedQueueing, recorder.stats().startedQueueing);
    ASSERT_EQ(forwarded.finishedQueueing, recorder.stats().finishedQueueing);
}

TEST_F(TicketAdmissionStatsTest, RecorderDeregistersOnDestruction) {
    auto opCtx = makeOperationContext();
    auto& admCtx = getAdmCtx(opCtx.get());

    int updates = 0;
    {
        ScopedTicketAdmissionStatsRecorder recorder(
            opCtx.get(), [&](const TicketAdmissionStats&) { ++updates; });
        admCtx.recordExecutionAcquisition(AdmissionContext::Priority::kNormal,
                                          ExecutionAdmissionContext::QueueType::kNormal);
        ASSERT_EQ(updates, 1);
    }

    ASSERT_EQ(admCtx.getTicketStatsRecorder(), nullptr);
    admCtx.recordExecutionAcquisition(AdmissionContext::Priority::kNormal,
                                      ExecutionAdmissionContext::QueueType::kNormal);
    ASSERT_EQ(updates, 1);

    // A new recorder can be registered afterwards and starts from zero.
    ScopedTicketAdmissionStatsRecorder recorder(opCtx.get(), nullptr);
    ASSERT_EQ(recorder.stats().admissions, 0);
    admCtx.recordExecutionAcquisition(AdmissionContext::Priority::kLow,
                                      ExecutionAdmissionContext::QueueType::kLow);
    ASSERT_EQ(recorder.stats().admissions, 1);
    ASSERT_EQ(recorder.stats().lowPriorityAdmissions, 1);
}

TEST_F(TicketAdmissionStatsTest, RecorderIgnoresExemptAdmissions) {
    auto opCtx = makeOperationContext();
    auto& admCtx = getAdmCtx(opCtx.get());

    ScopedTicketAdmissionStatsRecorder recorder(opCtx.get(), nullptr);

    ScopedAdmissionPriority<ExecutionAdmissionContext> exemptPriority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
    admCtx.recordExecutionStartQueueing();
    admCtx.recordExecutionAcquisition(AdmissionContext::Priority::kExempt,
                                      ExecutionAdmissionContext::QueueType::kNormal);
    admCtx.recordExecutionWaitedAcquisition(Microseconds{100},
                                            ExecutionAdmissionContext::QueueType::kNormal);
    admCtx.recordExecutionRelease(Microseconds{50});

    ASSERT_EQ(recorder.stats().admissions, 0);
    ASSERT_EQ(recorder.stats().timeQueuedMicros, 0);
    ASSERT_EQ(recorder.stats().timeProcessingMicros, 0);
    ASSERT_EQ(recorder.stats().startedQueueing, 0);
}

TEST_F(TicketAdmissionStatsTest, StatsSubtraction) {
    // Designated initializers so the test is independent of field declaration order.
    const TicketAdmissionStats a{.startedQueueing = 4,
                                 .finishedQueueing = 3,
                                 .admissions = 5,
                                 .releases = 4,
                                 .lowPriorityAdmissions = 2,
                                 .timeQueuedMicros = 100,
                                 .timeProcessingMicros = 50};
    const TicketAdmissionStats b{.startedQueueing = 2,
                                 .finishedQueueing = 1,
                                 .admissions = 2,
                                 .releases = 1,
                                 .lowPriorityAdmissions = 1,
                                 .timeQueuedMicros = 40,
                                 .timeProcessingMicros = 20};
    const auto delta = a - b;
    ASSERT_EQ(delta.timeQueuedMicros, 60);
    ASSERT_EQ(delta.timeProcessingMicros, 30);
    ASSERT_EQ(delta.admissions, 3);
    ASSERT_EQ(delta.releases, 3);
    ASSERT_EQ(delta.lowPriorityAdmissions, 1);
    ASSERT_EQ(delta.startedQueueing, 2);
    ASSERT_EQ(delta.finishedQueueing, 2);
}

using TicketAdmissionStatsDeathTest = TicketAdmissionStatsTest;

DEATH_TEST_F(TicketAdmissionStatsDeathTest, CannotRegisterTwoRecorders, "Invariant failure") {
    auto opCtx = makeOperationContext();
    ScopedTicketAdmissionStatsRecorder first(opCtx.get(), nullptr);
    ScopedTicketAdmissionStatsRecorder second(opCtx.get(), nullptr);
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
using TaskTypeDeathTest = TaskTypeTest;

DEATH_TEST_F(TaskTypeDeathTest, CannotSetDifferentTypes, "Invariant failure") {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setTaskType(opCtx.get(), TaskType::NonDeprioritizable);
    getAdmCtx(opCtx.get()).setTaskType(opCtx.get(), TaskType::Background);
}

DEATH_TEST_F(TaskTypeDeathTest, CannotSetAndScopeDifferentTypes, "Invariant failure") {
    auto opCtx = makeOperationContext();
    getAdmCtx(opCtx.get()).setTaskType(opCtx.get(), TaskType::NonDeprioritizable);
    ScopedTaskTypeBackground bg(opCtx.get());
}

DEATH_TEST_F(TaskTypeDeathTest, CannotNestDifferentTaskTypes, "Invariant failure") {
    auto opCtx = makeOperationContext();
    ScopedTaskTypeBackground bg(opCtx.get());
    ScopedTaskTypeNonDeprioritizable nd(opCtx.get());
}

DEATH_TEST_F(TaskTypeDeathTest, CannotCreateScopeWithLowPriority, "Invariant failure") {
    auto opCtx = makeOperationContext();
    ScopedAdmissionPriority<ExecutionAdmissionContext> lowPriority(
        opCtx.get(), AdmissionContext::Priority::kLow);
    ScopedTaskTypeBackground bg(opCtx.get());
}

DEATH_TEST_F(TaskTypeDeathTest, CannotCreateScopeInMultiDocTransaction, "Invariant failure") {
    auto opCtx = makeOperationContext();
    opCtx->setInMultiDocumentTransaction();
    ScopedTaskTypeBackground bg(opCtx.get());
}

#endif

}  // namespace
}  // namespace mongo::admission::execution_control
