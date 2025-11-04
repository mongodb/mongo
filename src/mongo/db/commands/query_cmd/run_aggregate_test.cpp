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
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

/**
 * This class is declared as a friend of OperationMemoryUsageTracker so it can access private
 * fields. As a result, it needs to be in the mongo namespace.
 */
class RunAggregateTest : service_context_test::WithSetupTransportLayer,
                         public DBCommandTestFixture {
protected:
    void setUp() override {
        DBCommandTestFixture::setUp();
        auto net = executor::makeNetworkInterface("RunAggregateTest");

        ThreadPool::Options options;
        auto pool = std::make_unique<ThreadPool>(options);

        _executor = executor::ThreadPoolTaskExecutor::create(std::move(pool), std::move(net));
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor.reset();
        DBCommandTestFixture::tearDown();
    }

    static OperationContext* getTrackerOpCtx(OperationMemoryUsageTracker* tracker) {
        return tracker->_opCtx;
    }

    /**
     * Generate a $trackingMock stage that generates 'nDocs' documents, with a simple schema:
     *   {a: 0, b: "aaaaaaaaaaaaaaaaaaaaaaaaaaa"}
     *   {a: 1, b: "aaaaaaaaaaaaaaaaaaaaaaaaaaa"}
     *   ...
     *
     * If 'errorOffset' is not boost::none, generate a special error document that will produce an
     * error when the stage is executed at the given offset:
     *   {error: "<msg>"}
     */
    BSONObj generateTrackingMockStage(size_t nDocs,
                                      boost::optional<size_t> errorOffset = boost::none) {
        BSONObjBuilder builder;
        BSONArrayBuilder arrayBuilder{builder.subarrayStart("$trackingMock")};
        StringData stringVal = "aaaaaaaaaaaaaaaaaaaaaaaaaaa"_sd;
        for (size_t i = 0; i < nDocs; ++i) {
            if (!errorOffset || *errorOffset != i) {
                arrayBuilder.append(BSON("a" << static_cast<int64_t>(i) << "b" << stringVal));
            } else {
                arrayBuilder.append(BSON("error" << "RunAggregateTest error"));
            }
        }
        arrayBuilder.done();

        return builder.obj();
    }

    /**
     * This function is assumed to be running on a new thread that can run concurrently with others,
     * as part of testing exchange pipelines. It calls getMore() repeatedly on the given cursor id,
     * and checks for expected results and/or an error.
     */
    static Status consume(ServiceContext* svcCtx,
                          size_t nConsumers,
                          size_t consumerId,
                          int64_t cursorId,
                          size_t nExpectedDocs,
                          bool expectError) {
        Status status = Status::OK();
        PseudoRandom prng(Date_t::now().asInt64());
        ThreadClient client = ThreadClient{svcCtx->getService()};

        BSONObj result;
        int docCount = 0;
        do {
            // Let's simulate how an actual client would do things, where there is a distinct opCtx
            // for each client request.
            ServiceContext::UniqueOperationContext opCtxOwned =
                svcCtx->makeOperationContext(client.get());
            OperationContext* opCtx = opCtxOwned.get();
            DBDirectClient dbdClient{opCtx};

            const size_t batchSize = 5;
            BSONObj getMoreCmdObj = fromjson(fmt::format(
                R"({{
                    getMore: {},
                    collection: "$cmd.aggregate",
                    batchSize: {}
                }})",
                cursorId,
                batchSize));
            bool success =
                dbdClient.runCommand(DBCommandTestFixture::kDatabaseName, getMoreCmdObj, result);

            if (!success && !expectError) {
                FAIL("Unexpected error: " + result.toString());
            } else if (!success && expectError) {
                assertBSONNotOk(result);
                auto errorCode = result["code"].Int();
                status =
                    Status{static_cast<ErrorCodes::Error>(errorCode), result["errmsg"].String()};
                // We expect there to be no tracker attached to the opCtx at this point, because we
                // always move it back to the Exchange object before returning.
                std::unique_ptr<OperationMemoryUsageTracker> tracker =
                    OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx);
                ASSERT_EQ(nullptr, tracker.get());
                break;
            }

            ASSERT_EQ(result["ok"].Number(), 1.0);

            cursorId = result["cursor"].Obj()["id"].Long();
            std::vector<BSONElement> docs = result["cursor"].Obj()["nextBatch"].Array();
            if (cursorId != 0) {
                // Documents are distributed round-robin, so we can predict which documents will be
                // received by each thread.
                ASSERT_LTE(docs.size(), batchSize);
                for (auto&& doc : docs) {
                    int64_t expectedA = docCount * nConsumers + consumerId;
                    ASSERT_BSONOBJ_EQ(
                        doc.Obj(),
                        BSON("a" << expectedA << "b" << "aaaaaaaaaaaaaaaaaaaaaaaaaaa"_sd));
                    docCount++;
                }
            } else {
                // No more documents.
                ASSERT_EQ(docs.size(), 0);
                // Unfortunately, the operation memory tracker stays attached to the Exchage object,
                // so we don't have a chance to examine it in this test.
                ASSERT_FALSE(OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx));
            }

            // Let some other threads do some work.
            sleepmillis(prng.nextInt32() % 20 + 1);
        } while (cursorId != 0);

        if (expectError) {
            ASSERT_FALSE(status.isOK());
        } else {
            ASSERT_EQ(docCount, nExpectedDocs);
        }

        return status;
    }

    /**
     * Assert a truthy "ok" field in a BSONObj. Some code paths create the "ok" field as a bool, but
     * more often it's a numeric field.
     *
     * TODO SERVER-110496: We shouldn't have to do this.
     */
    static void assertBSONOk(BSONObj bson) {
        BSONElement ok = bson["ok"];
        if (ok.type() == BSONType::boolean) {
            ASSERT_TRUE(ok.boolean());
        } else {
            ASSERT_NE(ok.Number(), 0.0);
        }
    }

    /**
     * Assert a falsey "ok" field in a BSONObj. Some code paths create the "ok" field as a bool, but
     * more often it's a numeric field.
     *
     * TODO SERVER-110496: We shouldn't have to do this.
     */
    static void assertBSONNotOk(BSONObj bson) {
        BSONElement ok = bson["ok"];
        if (ok.type() == BSONType::boolean) {
            ASSERT_FALSE(ok.boolean());
        } else {
            ASSERT_EQ(ok.Number(), 0.0);
        }
    }

    struct ExchangeTestParams {
        size_t nConsumers;
        size_t nTotalDocs;
        boost::optional<size_t> errorOffset = boost::none;
    };

    /**
     * Run a test that executes an exchange pipeline with the given number of consumers, etc. This
     * function starts one additional thread for each consumer.
     */
    void runExchangeMemoryTrackingTest(const ExchangeTestParams& params) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                                   true);
        // The exchange execution flow is to submit the initial aggregate() request with a batchSize
        // of 0, and then follow up with standard getMore() requests.
        auto aggCmdObj = fromjson(
            fmt::format(R"({{
                aggregate: 1,
                pipeline: [{}],
                cursor: {{batchSize: 0}},
                exchange: {{
                    "policy": "roundrobin",
                    "consumers": {},
                    "orderPreserving": false,
                    "bufferSize": 128,
                    "key": {{}}
                }}
            }})",
                        generateTrackingMockStage(params.nTotalDocs, params.errorOffset).toString(),
                        params.nConsumers));
        BSONObj res = runCommand(aggCmdObj.getOwned());

        using Handle = executor::TaskExecutor::CallbackHandle;
        std::vector<BSONElement> cursors;
        BSONObj oneConsumerCursor;
        if (params.nConsumers == 1) {
            // If there is just one consumer, we get back a single object.
            oneConsumerCursor = BSON("" << res).getOwned();
            cursors.push_back(oneConsumerCursor[""]);
        } else {
            // For multiple consumers, we get back an array.
            cursors = res["cursors"].Array();
        }

        // Spawn a new thread for each consumer to retrieve the result data, via the consume()
        // method.
        std::vector<Handle> handles;
        std::vector<Status> statuses{params.nConsumers, Status::OK()};
        size_t consumerId = 0;
        for (auto&& elem : cursors) {
            BSONObj cursor = elem.Obj();
            assertBSONOk(cursor);

            CursorId cursorId = cursor["cursor"].Obj()["id"].Long();
            StatusWith<Handle> swHandle =
                _executor->scheduleWork([consumerId, cursorId, params, &statuses, this](
                                            const executor::TaskExecutor::CallbackArgs& cbArgs) {
                    statuses[consumerId] = RunAggregateTest::consume(
                        getServiceContext(),
                        params.nConsumers,
                        consumerId,
                        cursorId,
                        params.nTotalDocs / params.nConsumers /* nExpectedDocs */,
                        params.errorOffset.has_value() /* expectError */);
                });
            ASSERT_OK(swHandle);
            handles.push_back(swHandle.getValue());
            consumerId++;
        }

        for (auto&& handle : handles) {
            _executor->wait(handle);
        }

        // If we expected an error make sure we got one internal error and the rest are passthrough
        // errors.
        if (params.errorOffset.has_value()) {
            size_t nInternalErrors = 0;
            for (const Status& status : statuses) {
                if (status.code() == ErrorCodes::InternalError) {
                    ++nInternalErrors;
                } else {
                    ASSERT_EQ(status.code(), ErrorCodes::ExchangePassthrough);
                }
            }
            ASSERT_EQ(1, nInternalErrors);
        }
    }

    std::shared_ptr<executor::TaskExecutor> _executor;
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
     * As a special case, if any of the documents have the form {error: "<msg>"}, then they will
     * produce an error instead of returning that document from doGetNext().
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
    void doDispose() final {
        // Some interesting edge cases have turned up when tracking memory
        // during dispose, since if we adjust the amount of memory in use after
        // the operation memory tracker has been destroyed, we will crash.
        _tracker.set(0);
    }

    /**
     * Override doGetNext to track memory usage for each document returned, and reset the in-use
     * memory bytes when EOF is reached.
     *
     * If any of the documents to be output have the form {error: "<msg>"}, this method will throw
     * an internal error with the given message.
     */
    GetNextResult doGetNext() override {
        GetNextResult result = MockStage::doGetNext();
        if (result.isAdvanced()) {
            const Document& doc = result.getDocument();
            FieldIterator it = doc.fieldIterator();
            if (it.more() && it.fieldName() == "error") {
                std::string errMsg = it.next().second.getString();
                uasserted(ErrorCodes::InternalError, errMsg);
            }
            _tracker.add(doc.getApproximateSize());
        } else if (result.isEOF()) {
            _tracker.add(-_tracker.inUseTrackedMemoryBytes());
        }

        return result;
    }

    SimpleMemoryUsageTracker _tracker;
};

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

namespace {

REGISTER_AGG_STAGE_MAPPING(trackingMockStage,
                           mongo::DocumentSourceTrackingMock::id,
                           documentSourceTrackingMockToStageFn)
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

TEST_F(RunAggregateTest, ExchangePipelineAndMemoryTrackingWorks1Consumer) {
    runExchangeMemoryTrackingTest({
        .nConsumers = 1,
        .nTotalDocs = 5,
    });
}

TEST_F(RunAggregateTest, ExchangePipelineAndMemoryTrackingWorksNConsumers) {
    runExchangeMemoryTrackingTest({
        .nConsumers = 5,
        .nTotalDocs = 500,
    });
}

TEST_F(RunAggregateTest, ExchangePipelineAndMemoryTrackingWorksNConsumersWithError) {
    runExchangeMemoryTrackingTest({
        .nConsumers = 5,
        .nTotalDocs = 500,
        .errorOffset = 400,
    });
}

}  // namespace
}  // namespace mongo
