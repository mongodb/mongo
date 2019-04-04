/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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
// This file contains tests for merizo/db/exec/queued_data_stage.cpp
//

#include "merizo/db/exec/queued_data_stage.h"

#include <boost/optional.hpp>

#include "merizo/db/exec/working_set.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/service_context_d_test_fixture.h"
#include "merizo/stdx/memory.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/clock_source_mock.h"

using namespace merizo;

namespace {

using std::unique_ptr;
using stdx::make_unique;

class QueuedDataStageTest : public ServiceContextMerizoDTest {
public:
    QueuedDataStageTest() {
        getServiceContext()->setFastClockSource(stdx::make_unique<ClockSourceMock>());
        _opCtx = makeOperationContext();
    }

protected:
    OperationContext* getOpCtx() {
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
    auto mock = make_unique<QueuedDataStage>(getOpCtx(), &ws);
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
TEST_F(QueuedDataStageTest, validateStats) {
    WorkingSet ws;
    WorkingSetID wsID;
    auto mock = make_unique<QueuedDataStage>(getOpCtx(), &ws);

    // make sure that we're at all zero
    const CommonStats* stats = mock->getCommonStats();
    ASSERT_EQUALS(stats->yields, 0U);
    ASSERT_EQUALS(stats->unyields, 0U);
    ASSERT_EQUALS(stats->works, 0U);
    ASSERT_EQUALS(stats->needTime, 0U);
    ASSERT_EQUALS(stats->advanced, 0U);
    ASSERT_FALSE(stats->isEOF);

    // 'perform' some operations, validate stats
    // needTime
    mock->pushBack(PlanStage::NEED_TIME);
    mock->work(&wsID);
    ASSERT_EQUALS(stats->works, 1U);
    ASSERT_EQUALS(stats->needTime, 1U);

    // advanced, with pushed data
    WorkingSetID id = ws.allocate();
    mock->pushBack(id);
    mock->work(&wsID);
    ASSERT_EQUALS(stats->works, 2U);
    ASSERT_EQUALS(stats->advanced, 1U);

    // yields
    mock->saveState();
    ASSERT_EQUALS(stats->yields, 1U);

    // unyields
    mock->restoreState();
    ASSERT_EQUALS(stats->unyields, 1U);


    // and now we are d1U, but must trigger EOF with getStats()
    ASSERT_FALSE(stats->isEOF);
    unique_ptr<PlanStageStats> allStats(mock->getStats());
    ASSERT_TRUE(stats->isEOF);
}
}
