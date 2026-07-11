// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/or.h"

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
