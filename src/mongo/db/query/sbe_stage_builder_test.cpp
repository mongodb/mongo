/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/shard_filterer_mock.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/db/query/shard_filterer_factory_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class SbeStageBuilderTest : public SbeStageBuilderTestFixture {
protected:
    std::unique_ptr<ShardFiltererFactoryInterface> makeAlwaysPassShardFiltererInterface() {
        return std::make_unique<ShardFiltererFactoryMock>(
            std::make_unique<ConstantFilterMock>(true, BSONObj{}));
    }
};

TEST_F(SbeStageBuilderTest, TestVirtualScan) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced.
    auto virtScan =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);

    // Make a QuerySolution from the root virtual scan node.
    auto querySolution = makeQuerySolution(std::move(virtScan));
    ASSERT_EQ(querySolution->root()->nodeId(), 1);

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data] =
        buildPlanStage(std::move(querySolution), true, std::move(shardFiltererInterface));
    auto resultAccessors = prepareTree(&data.ctx, stage.get(), resultSlots);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the recordIDs are what we expect.
        auto [tag, val] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tag == sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(index, sbe::value::bitcastTo<int64_t>(val));

        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[1]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));
        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

TEST_F(SbeStageBuilderTest, TestLimitOneVirtualScan) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a root limit node that takes ownership of a
    // VirtualScanNode.
    auto virtScan =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);
    auto limitNode = std::make_unique<LimitNode>();
    limitNode->children.push_back(virtScan.release());
    limitNode->limit = 1;

    // Make a QuerySolution from the root limitNode.
    auto querySolution = makeQuerySolution(std::move(limitNode));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data] =
        buildPlanStage(std::move(querySolution), true, std::move(shardFiltererInterface));

    // Prepare the sbe::PlanStage for execution.
    auto resultAccessors = prepareTree(&data.ctx, stage.get(), resultSlots);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the recordIDs are what we expect.
        auto [tag, val] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tag == sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(index, sbe::value::bitcastTo<int64_t>(val));

        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[1]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));

        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 1);
}

TEST_F(SbeStageBuilderTest, VirtualCollScanWithoutRecordId) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a root limit node that takes ownership of a
    // VirtualScanNode.
    auto virtScan =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);
    auto querySolution = makeQuerySolution(std::move(virtScan));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data] =
        buildPlanStage(std::move(querySolution), false, std::move(shardFiltererInterface));

    // Prepare the sbe::PlanStage for execution.
    auto resultAccessors = prepareTree(&data.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));

        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

TEST_F(SbeStageBuilderTest, VirtualIndexScan) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced.
    auto virtScan = std::make_unique<VirtualScanNode>(
        docs, VirtualScanNode::ScanType::kIxscan, true, BSON("a" << 1 << "b" << 1));
    auto querySolution = makeQuerySolution(std::move(virtScan));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data] =
        buildPlanStage(std::move(querySolution), true, std::move(shardFiltererInterface));
    auto resultAccessors = prepareTree(&data.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 2u);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the recordIDs are what we expect.
        auto [tag, val] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tag == sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(index, sbe::value::bitcastTo<int64_t>(val));

        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[1]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));
        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

TEST_F(SbeStageBuilderTest, VirtualIndexScanWithoutRecordId) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced.
    auto virtScan = std::make_unique<VirtualScanNode>(
        docs, VirtualScanNode::ScanType::kIxscan, false, BSON("a" << 1 << "b" << 1));
    auto querySolution = makeQuerySolution(std::move(virtScan));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data] =
        buildPlanStage(std::move(querySolution), false, std::move(shardFiltererInterface));
    auto resultAccessors = prepareTree(&data.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));
        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}
}  // namespace mongo
