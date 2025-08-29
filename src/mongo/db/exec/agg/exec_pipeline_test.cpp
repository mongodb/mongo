/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/exec_pipeline.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::test {

using Pipeline = mongo::exec::agg::Pipeline;
using Stage = mongo::exec::agg::Stage;
using StagePtr = mongo::exec::agg::StagePtr;
using StageContainer = mongo::exec::agg::Pipeline::StageContainer;
using GetNextResult = mongo::exec::agg::GetNextResult;

class FakeStage : public Stage {
public:
    FakeStage(const boost::intrusive_ptr<ExpressionContext>& expCtx) : Stage("$fake", expCtx) {}

    GetNextResult doGetNext() final {
        return GetNextResult::makeEOF();
    }

    Stage* getSource() {
        return pSource;
    }
};

TEST(PipelineTest, OneStagePipeline) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    StageContainer stages{make_intrusive<FakeStage>(expCtx)};

    // Assert that the source stage is initially set to nullptr.
    ASSERT_EQ(nullptr, dynamic_cast<FakeStage*>(stages.back().get())->getSource());

    Pipeline pl(std::move(stages), expCtx);

    // Assert that the source stage is still set to nullptr.
    ASSERT_EQ(nullptr, dynamic_cast<FakeStage*>(pl.getStages().back().get())->getSource());
}

TEST(PipelineTest, ThreeStagePipeline) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto stage0 = make_intrusive<FakeStage>(expCtx);
    auto stage1 = make_intrusive<FakeStage>(expCtx);
    auto stage2 = make_intrusive<FakeStage>(expCtx);

    StageContainer stages{stage0, stage1, stage2};

    Pipeline pl(std::move(stages), expCtx);

    // Assert that the stage order does not change.
    ASSERT_EQ(stage0.get(), pl.getStages()[0].get());
    ASSERT_EQ(stage1.get(), pl.getStages()[1].get());
    ASSERT_EQ(stage2.get(), pl.getStages()[2].get());
}

DEATH_TEST_REGEX(PipelineTest, GetNextResultOnEmptyPipelineThrows, "Tripwire assertion.*10549300") {
    StageContainer stages;
    ASSERT_THROWS_CODE(Pipeline(std::move(stages), make_intrusive<ExpressionContextForTest>()),
                       AssertionException,
                       10549300);
}

}  // namespace mongo::test
