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

//
// This file contains tests for mongo/db/exec/queued_data_stage.cpp
//

#include "mongo/db/exec/classic/queued_data_stage.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

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
