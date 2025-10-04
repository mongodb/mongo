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

#include "mongo/db/exec/classic/or.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace mongo {
namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class OrTest : public ServiceContextMongoDTest {
public:
    OrTest()
        : _opCtx{makeOperationContext()},
          _expCtx{ExpressionContextBuilder{}.opCtx(_opCtx.get()).ns(kNss).build()},
          _ws{} {}

    /**
     * Helper to create a QueuedDataStage with the given documents.
     */
    std::unique_ptr<QueuedDataStage> createQueuedDataStage(const std::vector<BSONObj>& documents) {

        auto queuedStage = std::make_unique<QueuedDataStage>(_expCtx.get(), &_ws);

        // Add documents to the queued stage
        for (const auto& doc : documents) {
            auto rid = doc["_id"]._numberInt();
            WorkingSetID wsid = _ws.allocate();
            WorkingSetMember* member = _ws.get(wsid);
            member->doc = {SnapshotId(), Document{doc}};
            member->recordId = RecordId{rid};
            member->transitionToRecordIdAndObj();

            queuedStage->pushBack(wsid);
        }

        return queuedStage;
    }

    /**
     * Execute the OrStage and collect all results.
     */
    std::vector<BSONObj> executeOrStage(OrStage& orStage, bool expectMemoryUsage) {
        std::vector<BSONObj> results;
        WorkingSetID wsid = WorkingSet::INVALID_ID;

        uint64_t peakTrackedMemoryBytes = 0;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state != PlanStage::IS_EOF) {
            state = orStage.work(&wsid);

            if (state == PlanStage::ADVANCED) {
                const auto& tracker = orStage.getMemoryTracker_forTest();
                uint64_t inUseTrackedMemoryBytes = tracker.inUseTrackedMemoryBytes();
                if (expectMemoryUsage) {
                    peakTrackedMemoryBytes =
                        std::max(inUseTrackedMemoryBytes, peakTrackedMemoryBytes);
                    // If we are deduping and we have processed a record, there should be non-zero
                    // memory usage.
                    ASSERT_GT(inUseTrackedMemoryBytes, 0);
                    ASSERT_GTE(static_cast<uint64_t>(tracker.peakTrackedMemoryBytes()),
                               peakTrackedMemoryBytes);
                } else {
                    ASSERT_EQ(0, inUseTrackedMemoryBytes);
                }

                WorkingSetMember* member = _ws.get(wsid);
                ASSERT_TRUE(member->hasObj());
                results.push_back(member->doc.value().toBson());
            }
        }

        return results;
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    WorkingSet _ws;
};

// Test basic OrStage functionality with no children.
TEST_F(OrTest, EmptyOrStageReturnsEOF) {
    const bool dedup = false;
    const MatchExpression* matchExpr = nullptr;
    OrStage orStage{_expCtx.get(), &_ws, dedup, matchExpr};

    // Should be EOF immediately since no children
    ASSERT_TRUE(orStage.isEOF());

    WorkingSetID wsid = WorkingSet::INVALID_ID;
    ASSERT_EQUALS(orStage.work(&wsid), PlanStage::IS_EOF);
}

// Test OrStage with one child stage.
TEST_F(OrTest, OrStageWithOneChildReturnsData) {
    const bool dedup = false;
    const MatchExpression* matchExpr = nullptr;
    OrStage orStage(_expCtx.get(), &_ws, dedup, matchExpr);

    // Create test documents.
    std::vector<BSONObj> childDocs = {BSON("_id" << 1 << "x" << 10),
                                      BSON("_id" << 2 << "x" << 20),
                                      BSON("_id" << 3 << "x" << 30)};

    // Add a child stage.
    auto childStage = createQueuedDataStage(childDocs);
    orStage.addChild(std::move(childStage));

    // Execute and verify results.
    const bool expectMemoryUsage = false;
    std::vector<BSONObj> results = executeOrStage(orStage, expectMemoryUsage);

    ASSERT_EQUALS(results.size(), childDocs.size());
    for (size_t i = 0; i < results.size(); ++i) {
        ASSERT_BSONOBJ_EQ(results[i], childDocs[i]);
    }
}

// Test OrStage with multiple child stages.
TEST_F(OrTest, OrStageWithMultipleChildrenReturnsUnion) {
    const bool dedup = false;
    const MatchExpression* matchExpr = nullptr;
    OrStage orStage{_expCtx.get(), &_ws, dedup, matchExpr};

    std::vector<BSONObj> child1Docs = {BSON("_id" << 1 << "x" << 10),
                                       BSON("_id" << 2 << "x" << 20)};
    std::vector<BSONObj> child2Docs = {BSON("_id" << 3 << "x" << 30),
                                       BSON("_id" << 4 << "x" << 40)};

    auto childStage1 = createQueuedDataStage(child1Docs);
    auto childStage2 = createQueuedDataStage(child2Docs);

    orStage.addChild(std::move(childStage1));
    orStage.addChild(std::move(childStage2));

    const bool expectMemoryUsage = false;
    std::vector<BSONObj> results = executeOrStage(orStage, expectMemoryUsage);

    // OrStage should return all results from both children.
    std::vector<BSONObj> expectedDocs = {BSON("_id" << 1 << "x" << 10),
                                         BSON("_id" << 2 << "x" << 20),
                                         BSON("_id" << 3 << "x" << 30),
                                         BSON("_id" << 4 << "x" << 40)};

    ASSERT_EQ(results.size(), expectedDocs.size());
    for (size_t i = 0; i < results.size(); ++i) {
        ASSERT_BSONOBJ_EQ(results[i], expectedDocs[i]);
    }
}

// Test OrStage with multiple child stages and deduplication.
TEST_F(OrTest, OrStageWithMultipleChildrenDedupes) {
    const bool dedup = true;
    const MatchExpression* matchExpr = nullptr;
    OrStage orStage{_expCtx.get(), &_ws, dedup, matchExpr};

    std::vector<BSONObj> child1Docs = {BSON("_id" << 1 << "x" << 10),
                                       BSON("_id" << 2 << "x" << 20),
                                       BSON("_id" << 3 << "x" << 30)};
    std::vector<BSONObj> child2Docs = {BSON("_id" << 2 << "x" << 20),
                                       BSON("_id" << 3 << "x" << 30),
                                       BSON("_id" << 4 << "x" << 40)};

    auto childStage1 = createQueuedDataStage(child1Docs);
    auto childStage2 = createQueuedDataStage(child2Docs);

    orStage.addChild(std::move(childStage1));
    orStage.addChild(std::move(childStage2));

    const bool expectMemoryUsage = true;
    std::vector<BSONObj> results = executeOrStage(orStage, expectMemoryUsage);

    // OrStage should return all results from both children, with duplicates excluded.
    std::vector<BSONObj> expectedDocs = {BSON("_id" << 1 << "x" << 10),
                                         BSON("_id" << 2 << "x" << 20),
                                         BSON("_id" << 3 << "x" << 30),
                                         BSON("_id" << 4 << "x" << 40)};

    ASSERT_EQ(results.size(), expectedDocs.size());
    for (size_t i = 0; i < results.size(); ++i) {
        ASSERT_BSONOBJ_EQ(results[i], expectedDocs[i]);
    }
}

}  // namespace
}  // namespace mongo
