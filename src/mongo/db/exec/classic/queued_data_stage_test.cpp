// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

//
// This file contains tests for mongo/db/exec/queued_data_stage.cpp
//

#include "mongo/db/exec/classic/queued_data_stage.h"

#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>


using namespace mongo;

namespace {

using std::unique_ptr;

const static NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class QueuedDataStageTest : public ServiceContextMongoDTest {
public:
    QueuedDataStageTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {
        _opCtx = makeOperationContext();
    }

protected:
    OperationContext* opCtx() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

//
// Basic test that we get out valid stats objects.
//
TEST_F(QueuedDataStageTest, getValidStats) {
    WorkingSet ws;
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx()).ns(kNss).build();
    auto mock = std::make_unique<QueuedDataStage>(expCtx.get(), &ws);
    const CommonStats* commonStats = mock->getCommonStats();
    ASSERT_EQUALS(commonStats->works, static_cast<size_t>(0));
    const SpecificStats* specificStats = mock->getSpecificStats();
    ASSERT(specificStats);
    unique_ptr<PlanStageStats> allStats(mock->getStats());
    ASSERT_EQUALS(allStats->stageType, mock->stageType());
}

//
// Test that our stats are updated as we perform operations.
//
TEST_F(QueuedDataStageTest, ValidateStats) {
    WorkingSet ws;
    WorkingSetID wsID;
    const auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx()).ns(kNss).build();

    auto mock = std::make_unique<QueuedDataStage>(expCtx.get(), &ws);

    // make sure that we're at all zero
    const CommonStats* stats = mock->getCommonStats();
    ASSERT_EQUALS(stats->yields, 0U);
    ASSERT_EQUALS(stats->unyields, 0U);
    ASSERT_EQUALS(stats->works, 0U);
    ASSERT_EQUALS(stats->needTime, 0U);
    ASSERT_EQUALS(stats->advanced, 0U);
    ASSERT_FALSE(stats->isEOF);

    // advanced, with pushed data
    WorkingSetID id = ws.allocate();
    mock->pushBack(id);
    mock->work(&wsID);
    ASSERT_EQUALS(stats->works, 1U);
    ASSERT_EQUALS(stats->advanced, 1U);

    // yields
    mock->saveState();
    ASSERT_EQUALS(stats->yields, 1U);

    // unyields
    mock->restoreState({nullptr});
    ASSERT_EQUALS(stats->unyields, 1U);


    // and now we are d1U, but must trigger EOF with getStats()
    ASSERT_FALSE(stats->isEOF);
    unique_ptr<PlanStageStats> allStats(mock->getStats());
    ASSERT_TRUE(stats->isEOF);
}
}  // namespace
