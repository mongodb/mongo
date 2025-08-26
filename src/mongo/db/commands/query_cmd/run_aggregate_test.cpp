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

#include "mongo/db/commands/query_cmd/run_aggregate.h"

#include "mongo/bson/json.h"
#include "mongo/db/commands/db_command_test_fixture.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

/**
 * This class is declared as a friend of OperationMemoryUsageTracker so it can access private
 * fields. As a result, it needs to be in the mongo namespace.
 */
class RunAggregateTest : public DBCommandTestFixture {
protected:
    static OperationContext* getTrackerOpCtx(OperationMemoryUsageTracker* tracker) {
        return tracker->_opCtx;
    }
};

namespace {
/**
 * This is a subclass of DocumentSourceMock that will track the memory of each document it produces,
 * and reset the in-use memory bytes to zero when EOF is reached.
 *
 * We add support for parsing this method here so that we can invoke `aggregate()` in C++.
 */
class DocumentSourceTrackingMock : public DocumentSourceMock {
public:
    static constexpr StringData kStageName = "$trackingMock"_sd;

    /**
     * Give this mock stage a syntax like this:
     *     {$trackingMock: [ {_id: 1, ... }, {_id: 2, ...} ]}
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
        std::deque<GetNextResult> results;
        std::vector<BSONElement> elems = elem.Array();
        for (auto& doc : elems) {
            results.emplace_back(Document{doc.Obj().getOwned()});
        }

        return boost::intrusive_ptr<DocumentSourceTrackingMock>{
            new DocumentSourceTrackingMock{results, pExpCtx}};
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    const char* getSourceName() const override {
        return kStageName.data();
    }

    /**
     * Produce constraints consistent with a stage that takes no inputs and produces documents at
     * the beginning of a pipeline.
     */
    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints = DocumentSourceMock::constraints(pipeState);
        constraints.requiredPosition = PositionRequirement::kFirst;
        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

private:
    /**
     * When constructing this stage, create the memory tracker with a factory method so that it
     * reports memory usage up to the operation-scoped memory tracker.
     */
    DocumentSourceTrackingMock(std::deque<GetNextResult> results,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock{std::move(results), expCtx} {}

    SimpleMemoryUsageTracker _tracker;
};

REGISTER_DOCUMENT_SOURCE(trackingMock,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceTrackingMock::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(trackingMock, DocumentSourceTrackingMock::id)

class TrackingMockStage : public mongo::exec::agg::MockStage {
    using GetNextResult = exec::agg::GetNextResult;

public:
    TrackingMockStage(StringData stageName,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      std::deque<GetNextResult> results)
        : mongo::exec::agg::MockStage(stageName, expCtx, std::move(results)),
          _tracker{OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(*expCtx)} {}

private:
    /**
     * Override doGetNext to track memory usage for each document returned, and reset the in-use
     * memory bytes when EOF is reached.
     */
    GetNextResult doGetNext() override {
        GetNextResult result = MockStage::doGetNext();
        if (result.isAdvanced()) {
            _tracker.add(result.getDocument().getApproximateSize());
        } else if (result.isEOF()) {
            _tracker.add(-_tracker.inUseTrackedMemoryBytes());
        }

        return result;
    }

    SimpleMemoryUsageTracker _tracker;
};

/**
 * Test that when we have memory tracking turned on, queries producing memory statistics will
 * transfer OperationMemoryUsageTracker to the cursor between the initial request and subsequent
 * getMore()s.
 */
TEST_F(RunAggregateTest, TransferOperationMemoryUsageTracker) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);
    auto aggCmdObj = fromjson(R"({
        aggregate: 1,
        pipeline: [{$trackingMock: [
            {_id: 1, foo: 10},
            {_id: 2, foo: 20},
            {_id: 3, foo: 30}
        ]}],
        cursor: {batchSize: 1}
    })");
    std::vector<BSONElement> expectedDocs =
        aggCmdObj["pipeline"].Obj().firstElement().Obj()["$trackingMock"].Array();

    BSONObj res = runCommand(aggCmdObj.getOwned());
    ASSERT_EQ(res["ok"].Number(), 1.0);

    std::vector<BSONElement> docs = res["cursor"].Obj()["firstBatch"].Array();
    auto expectedIt = expectedDocs.begin();
    int64_t cursorId = res["cursor"].Obj()["id"].Long();
    int64_t prevMemoryInUse = 0;
    while (cursorId != 0) {
        ASSERT_EQ(docs.size(), 1);
        ASSERT_BSONOBJ_EQ(docs[0].Obj(), expectedIt->Obj());
        ++expectedIt;

        {
            // The initial request and subsequent getMore()s should conclude with attaching the
            // memory tracker to the cursor.

            // The pin retrieved from the cursor manager has RAII semantics and will be released at
            // the end of this block. We need a new block here so the cursor isn't considered as
            // being in use when we call getMore() below.
            CursorManager* cursorManager = CursorManager::get(opCtx->getServiceContext());
            ClientCursorPin pin =
                unittest::assertGet(cursorManager->pinCursor(opCtx, cursorId, "getMore"));
            std::unique_ptr<OperationMemoryUsageTracker> tracker =
                OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx);
            ASSERT(tracker);
            ASSERT_EQ(getTrackerOpCtx(tracker.get()), nullptr);
            // $trackingMock will always be increasing memory count with each document returned, so
            // the max will always be the same as the current.
            ASSERT_GT(tracker->inUseTrackedMemoryBytes(), prevMemoryInUse);
            ASSERT_EQ(tracker->peakTrackedMemoryBytes(), tracker->inUseTrackedMemoryBytes());

            prevMemoryInUse = tracker->inUseTrackedMemoryBytes();

            OperationMemoryUsageTracker::moveToOpCtxIfAvailable(opCtx, std::move(tracker));
        }

        BSONObj getMoreCmdObj = fromjson(fmt::format(
            R"({{
                getMore: {},
                collection: "$cmd.aggregate",
                batchSize: 1
            }})",
            cursorId));
        res = runCommand(getMoreCmdObj.getOwned());
        ASSERT_EQ(res["ok"].Number(), 1.0);

        cursorId = res["cursor"].Obj()["id"].Long();
        docs = res["cursor"].Obj()["nextBatch"].Array();
    }

    ASSERT_EQ(expectedIt, expectedDocs.end());
    ASSERT_EQ(docs.size(), 0);
}

TEST_F(RunAggregateTest, MemoryTrackerWithinSubpipelineIsProperlyDestroyedOnKillCursor) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);

    // Set up the collection.
    BSONArrayBuilder docsBuilder;
    for (size_t i = 0; i < 10; ++i) {
        docsBuilder.append(fromjson(fmt::format("{{id: {}, val: {}}}", i, i)));
    }
    auto insertCmdObj = BSON("insert" << "coll"
                                      << "documents" << docsBuilder.arr() << "ordered" << true);
    BSONObj res = runCommand(insertCmdObj.getOwned());
    ASSERT_EQ(res["ok"].Number(), 1.0);
    ASSERT_EQ(res["n"].Int(), 10);

    // Create a pipeline with a memory-tracked stage ($group) within a subpipeline.
    auto aggCmdObj = fromjson(R"({
        aggregate: "coll",
        pipeline: [
            {
                $facet: {
                    grouped: [
                        { $group: {
                            _id: null,
                            sum: { $sum: "$val" }
                        }}
                    ]
                }
            }
        ],
        cursor: { batchSize: 1 }
    })");
    res = runCommand(aggCmdObj.getOwned());
    ASSERT_EQ(res["ok"].Number(), 1.0);
    ASSERT_BSONOBJ_EQ(res["cursor"]["firstBatch"].Array()[0].Obj(),
                      BSON("grouped" << BSON_ARRAY(BSON("_id" << BSONNULL << "sum" << 45))));

    // Sending a killCursor command to the aggregation should safely dispose of the memory tracker
    // without crashing.
    int64_t cursorId = res["cursor"].Obj()["id"].Long();
    auto killCursorCmdObj = BSON("killCursors" << "coll"
                                               << "cursors" << BSON_ARRAY(cursorId));
    res = runCommand(killCursorCmdObj.getOwned());
    ASSERT_EQ(res["ok"].Number(), 1.0);
    auto cursorsKilled = res["cursorsKilled"].Array();
    ASSERT_TRUE(cursorsKilled.size() == 1 && cursorsKilled[0].Long() == cursorId);
}
}  // namespace

// We have to define the mapping function outside the anonymous namespace in order to access private
// members of the DocumentSourceMock class.
boost::intrusive_ptr<exec::agg::Stage> documentSourceTrackingMockToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* dsMock = dynamic_cast<DocumentSourceTrackingMock*>(documentSource.get());

    tassert(10812602, "expected 'DocumentSourceTrackingMock' type", dsMock);

    return make_intrusive<TrackingMockStage>(
        dsMock->kStageName, dsMock->getExpCtx(), dsMock->_results);
}

REGISTER_AGG_STAGE_MAPPING(trackingMockStage,
                           mongo::DocumentSourceTrackingMock::id,
                           documentSourceTrackingMockToStageFn)

}  // namespace mongo
