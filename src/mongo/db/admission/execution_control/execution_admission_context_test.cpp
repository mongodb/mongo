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

#include "mongo/db/admission/execution_control/execution_admission_context.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_type_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo::admission::execution_control {
namespace {

constexpr StringData kFieldName = "executionAdmissionContextType"_sd;

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
    RAIIServerParameterControllerForTest featureFlagDisabled(
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
    RAIIServerParameterControllerForTest featureFlagDisabled(
        "featureFlagExecutionControlRemoteSpecification", true);
    auto opCtx = makeOperationContext();

    BSONObjBuilder builder;
    getAdmCtx(opCtx.get()).writeAsMetadata(opCtx.get(), &builder);

    ASSERT_FALSE(builder.asTempObj().hasField(kFieldName));
}

TEST_F(TaskTypeTest, WriteMetadataLowPriority) {
    RAIIServerParameterControllerForTest featureFlagDisabled(
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
    RAIIServerParameterControllerForTest featureFlagDisabled(
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
    RAIIServerParameterControllerForTest featureFlagDisabled(
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
    RAIIServerParameterControllerForTest featureFlagDisabled(
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
    RAIIServerParameterControllerForTest featureFlagDisabled(
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
