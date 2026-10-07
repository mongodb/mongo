// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/cursor_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
#include "mongo/db/pipeline/document_source_cursor_test_util.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

/**
 * Mock implementation of a PlanExecutor which randomly throws on calls to saveState() and
 * restoreState() based on given probabilities.
 */
class ThrowyPlanExecutor : public test::MockPlanExecutorBase {
public:
    ThrowyPlanExecutor(OperationContext* opCtx,
                       std::vector<BSONObj> data,
                       double saveProbability,
                       double restoreProbability,
                       std::shared_ptr<bool> holdsStorageEngineState)
        : test::MockPlanExecutorBase(opCtx, data),
          _seed(Date_t::now().asInt64()),
          _prng(_seed),
          _probabilityThrowInSave(saveProbability),
          _probabilityThrowInRestore(restoreProbability),
          _holdsStorageEngineState(holdsStorageEngineState) {
        LOGV2(10271308, "Created throwy plan executor with seed {seed}", "seed"_attr = _seed);
    }

    ~ThrowyPlanExecutor() override {
        *_holdsStorageEngineState = false;
    }

    // Generally the order of calls to detach the executor from storage:
    // saveState()
    // detachFromOperationContext() // This actually releases storage engine state.
    //
    // reattachToOperationContext()
    // restoreState() // This actually re-acquires storage engine state.
    //
    // The lack of symmetry here is no doubt confusing, but this is how it works.
    void saveState() override {
        // Note: It is NOT a precondition that the executor must be in a restored state to call
        // save(). save() is re-entrant and idempotent, specifically for the case where exceptions
        // are thrown in save() and restore().

        if (_prng.nextCanonicalDouble() < _probabilityThrowInSave) {
            throwWriteConflictException("WCE for test: saveState()");
        }
        _saved = true;
    }

    void restoreState(const RestoreContext& context) override {
        ASSERT(_saved);

        if (_prng.nextCanonicalDouble() < _probabilityThrowInRestore) {
            throwWriteConflictException("WCE for test: restoreState()");
        }
        _saved = false;
        *_holdsStorageEngineState = true;
    }

    void detachFromOperationContext() override {
        if (_saved) {
            *_holdsStorageEngineState = false;
        }
    }

    void reattachToOperationContext(OperationContext* opCtx) override {}

private:
    int64_t _seed;
    PseudoRandom _prng;
    double _probabilityThrowInSave;
    double _probabilityThrowInRestore;

    std::shared_ptr<bool> _holdsStorageEngineState;

    bool _saved = false;
};

using DSCursorTest = AggregationContextFixture;
TEST_F(DSCursorTest, TestSaveAndRestoreThrowing) {
    std::vector<BSONObj> bsons;
    for (size_t i = 0; i < 1000; ++i) {
        bsons.push_back(BSON("foo" << std::to_string(i)));
    }

    struct ThrowProbability {
        double saveProbability;
        double restoreProbability;
    };

    // Test with a variety of executors that throw with different probability.
    std::vector<double> vals = {0.0, 0.001, 0.01, 0.1, 1};
    std::vector<ThrowProbability> throwProbabilities;
    for (size_t i = 0; i < vals.size(); ++i) {
        for (size_t j = 0; j < vals.size(); ++j) {
            throwProbabilities.push_back({vals[i], vals[j]});
        }
    }

    for (auto probabilities : throwProbabilities) {
        size_t testsThatThrew = 0;
        size_t testsThatDidNotThrow = 0;

        for (size_t i = 0; i < 100; ++i) {
            // This is used to track whether the executor holds storage engine state. As part of
            // the test, we want to assert that the executor is NOT holding storage engine
            // resources when the catalog resources are freed.
            auto holdsStorageEngineStateBool = std::make_shared<bool>(true);

            auto exec = std::make_unique<ThrowyPlanExecutor>(getOpCtx(),
                                                             bsons,
                                                             probabilities.saveProbability,
                                                             probabilities.restoreProbability,
                                                             holdsStorageEngineStateBool);

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> execWithDeleter(
                std::move(exec).release(), PlanExecutor::Deleter{getOpCtx()});

            try {
                MultipleCollectionAccessor collections;  // Intentionally empty.
                auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
                auto cursor =
                    DocumentSourceCursor::create(std::move(execWithDeleter),
                                                 getExpCtx(),
                                                 DocumentSourceCursor::CursorType::kRegular);
                cursor->bindCatalogInfo(collections, stasher);
                // Overwrite the exisitng catalogResourceHandle with our test version.
                cursor->setCatalogResourceHandle_forTest(
                    make_intrusive<test::MockDSCursorCatalogResourceHandle>(
                        holdsStorageEngineStateBool));
                auto cursorStage = exec::agg::buildStage(cursor);

                for (const auto& expectedBson : bsons) {
                    DocumentSource::GetNextResult next = cursorStage->getNext();
                    ASSERT(next.isAdvanced());
                    auto doc = next.getDocument();
                    ASSERT_BSONOBJ_EQ(doc.toBson(), expectedBson);
                }

                DocumentSource::GetNextResult next = cursorStage->getNext();
                ASSERT(next.isEOF());

                cursorStage->dispose();
                testsThatDidNotThrow++;
            } catch (const DBException&) {
                // We're allowed to throw exceptions here due to the save()/restore() functions
                // throwing. However, we cannot crash the server.

                // If the test throws, we move on to the next one.
                testsThatThrew++;
            }
        }

        LOGV2(10271303,
              "Tests that threw {testsThatThrew}, tests that did not throw {testsThatDidNotThrow}",
              "testsThatThrew"_attr = testsThatThrew,
              "testsThatDidNotThrow"_attr = testsThatDidNotThrow);
    }
}

/**
 * MockPlanExecutor instance used for count-only queries.
 */
class MockPlanExecutorForCount : public test::MockPlanExecutorBase {
public:
    MockPlanExecutorForCount(OperationContext* opCtx,
                             std::vector<BSONObj> data,
                             std::shared_ptr<bool> calledGetNextWithoutOutput)
        : test::MockPlanExecutorBase(opCtx, std::move(data)),
          _calledGetNextWithoutOutput(std::move(calledGetNextWithoutOutput)) {}

    ExecState getNext(BSONObj* out, RecordId* dlOut) override {
        invariant(out == nullptr);
        *_calledGetNextWithoutOutput = true;
        return nextDocument() ? ExecState::ADVANCED : ExecState::IS_EOF;
    }

    void saveState() override {}

    void restoreState(const RestoreContext& context) override {}

    void detachFromOperationContext() override {}

    void reattachToOperationContext(OperationContext* opCtx) override {}

private:
    std::shared_ptr<bool> _calledGetNextWithoutOutput;
};

TEST_F(DSCursorTest, EmptyDocumentsUsesCountOnlyPlanExecutorInterface) {
    std::vector<BSONObj> bsons = {BSON("foo" << 1), BSON("foo" << 2), BSON("foo" << 3)};
    auto calledGetNextWithoutOutput = std::make_shared<bool>(false);
    auto exec =
        std::make_unique<MockPlanExecutorForCount>(getOpCtx(), bsons, calledGetNextWithoutOutput);
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> execWithDeleter(
        std::move(exec).release(), PlanExecutor::Deleter{getOpCtx()});

    MultipleCollectionAccessor collections;
    auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
    auto cursor = DocumentSourceCursor::create(
        std::move(execWithDeleter), getExpCtx(), DocumentSourceCursor::CursorType::kEmptyDocuments);
    cursor->bindCatalogInfo(collections, stasher);
    cursor->setCatalogResourceHandle_forTest(
        make_intrusive<test::MockDSCursorCatalogResourceHandle>(std::make_shared<bool>(false)));

    auto cursorStage = exec::agg::buildStage(cursor);
    for (size_t i = 0; i < bsons.size(); ++i) {
        auto next = cursorStage->getNext();
        ASSERT(next.isAdvanced());
        ASSERT(next.getDocument().toBson().isEmpty());
    }
    ASSERT(cursorStage->getNext().isEOF());
    ASSERT(*calledGetNextWithoutOutput);
    cursorStage->dispose();
}

/**
 * Builds a $cursor stage over 'bsons' with the given resume tracking behaviour, reporting
 * 'latestOplogTimestamp' as the position of the underlying scan.
 */
boost::intrusive_ptr<exec::agg::CursorStage> makeOplogScanCursorStage(
    OperationContext* opCtx,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::vector<BSONObj> bsons,
    Timestamp latestOplogTimestamp,
    DocumentSourceCursor::ResumeTrackingType resumeTrackingType) {
    auto exec = std::make_unique<test::MockPlanExecutorForOplogScan>(
        opCtx, std::move(bsons), latestOplogTimestamp);
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> execWithDeleter(
        std::move(exec).release(), PlanExecutor::Deleter{opCtx});

    MultipleCollectionAccessor collections;
    auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
    auto cursor = DocumentSourceCursor::create(std::move(execWithDeleter),
                                               expCtx,
                                               DocumentSourceCursor::CursorType::kRegular,
                                               resumeTrackingType);
    cursor->bindCatalogInfo(collections, stasher);
    cursor->setCatalogResourceHandle_forTest(
        make_intrusive<test::MockDSCursorCatalogResourceHandle>(std::make_shared<bool>(false)));
    auto stage = exec::agg::buildStage(cursor);
    auto* cursorStage = dynamic_cast<exec::agg::CursorStage*>(stage.get());
    ASSERT_TRUE(cursorStage);
    return boost::intrusive_ptr<exec::agg::CursorStage>(cursorStage);
}

TEST_F(DSCursorTest, ReportsNoHighWaterMarkClusterTimeBeforeTheFirstBatchIsLoaded) {
    auto cursorStage = makeOplogScanCursorStage(getOpCtx(),
                                                getExpCtx(),
                                                {BSON("ts" << Timestamp(100, 1))},
                                                Timestamp(200, 1),
                                                DocumentSourceCursor::ResumeTrackingType::kOplog);

    // Nothing has been read yet, so we know nothing about where the scan is.
    ASSERT_FALSE(cursorStage->getHighWaterMarkClusterTime());

    cursorStage->dispose();
}

TEST_F(DSCursorTest, ReportsHighWaterMarkClusterTimeForAResumableOplogScan) {
    auto cursorStage = makeOplogScanCursorStage(getOpCtx(),
                                                getExpCtx(),
                                                {BSON("ts" << Timestamp(100, 1))},
                                                Timestamp(200, 1),
                                                DocumentSourceCursor::ResumeTrackingType::kOplog);

    // While there is a document to return, the reported position is that document's timestamp: the
    // scan may have read further ahead, but the documents it read have not been returned yet.
    ASSERT_TRUE(cursorStage->getNext().isAdvanced());
    ASSERT_EQ(*cursorStage->getHighWaterMarkClusterTime(), Timestamp(100, 1));

    // Once the batch is drained we can report the position the scan itself reached, which is what
    // makes the high water mark advance over oplog entries which produce no result.
    ASSERT_TRUE(cursorStage->getNext().isEOF());
    ASSERT_EQ(*cursorStage->getHighWaterMarkClusterTime(), Timestamp(200, 1));

    cursorStage->dispose();
}

TEST_F(DSCursorTest, ReportsNoHighWaterMarkClusterTimeForANonResumableScan) {
    auto cursorStage = makeOplogScanCursorStage(getOpCtx(),
                                                getExpCtx(),
                                                {BSON("ts" << Timestamp(100, 1))},
                                                Timestamp(200, 1),
                                                DocumentSourceCursor::ResumeTrackingType::kNone);

    // This scan is not resumable, so its position says nothing about which events have been
    // produced and must not be reported as a high water mark.
    ASSERT_TRUE(cursorStage->getNext().isAdvanced());
    ASSERT_FALSE(cursorStage->getHighWaterMarkClusterTime());
    ASSERT_TRUE(cursorStage->getNext().isEOF());
    ASSERT_FALSE(cursorStage->getHighWaterMarkClusterTime());

    cursorStage->dispose();
}

}  // namespace
}  // namespace mongo
