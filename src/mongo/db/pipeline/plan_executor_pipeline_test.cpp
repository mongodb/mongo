// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/plan_executor_pipeline.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_test_optimizations.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/tick_source_mock.h"

#include <deque>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class TimedDisposeDocumentSource final : public DocumentSourceTestOptimizations {
public:
    using DocumentSourceTestOptimizations::DocumentSourceTestOptimizations;
    static const Id& id;
    Id getId() const override {
        return id;
    }
};

ALLOCATE_DOCUMENT_SOURCE_ID(timedDisposeDocumentSource, TimedDisposeDocumentSource::id);

class TimedDisposeStage final : public exec::agg::Stage {
public:
    explicit TimedDisposeStage(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : Stage("$timedDispose", expCtx) {}

private:
    exec::agg::GetNextResult doGetNext() override {
        return exec::agg::GetNextResult::makeEOF();
    }

    void doDispose() override {
        auto* ticks = static_cast<TickSourceMock<>*>(
            pExpCtx->getOperationContext()->getServiceContext()->getTickSource());
        ticks->advance(Milliseconds{100});
    }
};

using exec::agg::registerDocumentSourceToStageFn;
REGISTER_AGG_STAGE_MAPPING(timedDisposeStage,
                           TimedDisposeDocumentSource::id,
                           [](const boost::intrusive_ptr<DocumentSource>& ds)
                               -> boost::intrusive_ptr<exec::agg::Stage> {
                               return make_intrusive<TimedDisposeStage>(ds->getExpCtx());
                           });

class PlanExecutorTest : public unittest::Test {
protected:
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildExecutor() {
        // Create a control event
        auto doc = Document{{"test", true}};
        MutableDocument docBuilder(doc);
        docBuilder.metadata().setChangeStreamControlEvent(
            DocumentMetadataFields::ChangeStreamControlEventType::kTopology);

        auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
        auto pipeline = Pipeline::create({mock}, _expCtx);

        return plan_executor_factory::make(_expCtx, std::move(pipeline));
    }

    QueryTestServiceContext _serviceContext{};
    ServiceContext::UniqueOperationContext _opCtx = _serviceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContextForTest>(_opCtx.get());
};

/**
 * Test that the plan executor throws upon seeing a control event on a router.
 */
using PlanExecutorTestDeathTest = PlanExecutorTest;
DEATH_TEST_REGEX_F(PlanExecutorTestDeathTest,
                   AssertsOnControlEventOnRouter,
                   "Tripwire assertion.*10358906") {
    _expCtx->setInRouter(true);

    auto exec = buildExecutor();

    BSONObj objOut;
    ASSERT_THROWS_CODE(exec->getNext(&objOut, nullptr), AssertionException, 10358906);
}

/**
 * Test that the plan executor does not throw upon seeing a control event on a shard.
 */
TEST_F(PlanExecutorTest, DoesNotAssertsOnControlEventOnShard) {
    _expCtx->setInRouter(false);

    // Must be set to true to indicate we are on a shard. Otherwise document metadata is not going
    // to be serialized.
    _expCtx->setForPerShardCursor(true);

    auto exec = buildExecutor();

    BSONObj objOut;
    auto state = exec->getNext(&objOut, nullptr);
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, state);
    ASSERT_TRUE(objOut.hasField(Document::metaFieldChangeStreamControlEvent));
}

namespace {

ResumeToken makeEventToken(Timestamp ts) {
    return ResumeToken(ResumeTokenData{ts,
                                       ResumeTokenData::kDefaultTokenVersion,
                                       /*txnOpIndex=*/0,
                                       /*uuid=*/boost::none,
                                       /*eventIdentifier=*/Value(Document{{"_id", 1}}),
                                       ResumeTokenData::kNotFromInvalidate,
                                       ResumeTokenData::kEventToken});
}

ResumeToken makeEventTokenV(Timestamp ts, int version, const UUID& uuid) {
    return ResumeToken(ResumeTokenData{ts,
                                       version,
                                       /*txnOpIndex=*/0,
                                       /*uuid=*/boost::optional<UUID>{uuid},
                                       /*eventIdentifier=*/Value(Document{{"_id", 1}}),
                                       ResumeTokenData::kNotFromInvalidate,
                                       ResumeTokenData::kEventToken});
}

ResumeToken makeFragmentToken(Timestamp ts, const UUID& uuid, size_t fragmentNum) {
    ResumeTokenData tokenData{ts,
                              ResumeTokenData::kDefaultTokenVersion,
                              /*txnOpIndex=*/0,
                              /*uuid=*/boost::optional<UUID>{uuid},
                              /*eventIdentifier=*/Value(Document{{"_id", 1}}),
                              ResumeTokenData::kNotFromInvalidate,
                              ResumeTokenData::kEventToken};
    tokenData.fragmentNum = fragmentNum;
    return ResumeToken(tokenData);
}

ResumeToken makeInvalidateToken(Timestamp ts, const UUID& uuid, bool fromInvalidate) {
    return ResumeToken(ResumeTokenData{ts,
                                       ResumeTokenData::kDefaultTokenVersion,
                                       /*txnOpIndex=*/0,
                                       /*uuid=*/boost::optional<UUID>{uuid},
                                       /*eventIdentifier=*/Value(Document{{"_id", 1}}),
                                       fromInvalidate ? ResumeTokenData::kFromInvalidate
                                                      : ResumeTokenData::kNotFromInvalidate,
                                       ResumeTokenData::kEventToken});
}

// Marks the stream as one the router rebuilt for an end-of-transaction lookback, whose oplog scan
// starts before the resume point. Only such a stream may observe events that sort before the start
// point.
void enableBackfilling(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    DocumentSourceChangeStreamSpec spec;
    spec.setBackfillingStartAtOperationTime(Timestamp(1, 1));
    expCtx->setChangeStreamSpec(spec);
}

}  // namespace

// A client's starting token may be of a lower version than the stream's own event tokens. The
// position comparison must not be confused by the version difference.
TEST_F(PlanExecutorTest, AdvancesForEventAtStartPointWithHigherVersion) {
    const UUID uuid = UUID::gen();
    const auto startToken = makeEventTokenV(Timestamp(100, 1), 1, uuid);
    const auto eventToken = makeEventTokenV(Timestamp(100, 1), 2, uuid);

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(eventToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(eventToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));
    // The event is at the start point, so the PBRT advances to it even though its token version
    // differs from the start token's.
    ASSERT_BSONOBJ_EQ(eventToken.toBSON(), exec->getPostBatchResumeToken());
}

// A client's starting token may be of a higher version than the stream's own event tokens. The
// position comparison must not be confused by the version difference.
TEST_F(PlanExecutorTest, AdvancesForEventAtStartPointWithLowerVersion) {
    const UUID uuid = UUID::gen();
    const auto startToken = makeEventTokenV(Timestamp(100, 1), 2, uuid);
    const auto eventToken = makeEventTokenV(Timestamp(100, 1), 1, uuid);

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(eventToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(eventToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));
    // The event is at the start point, so the PBRT advances to it even though its token version
    // is lower than the start token's.
    ASSERT_BSONOBJ_EQ(eventToken.toBSON(), exec->getPostBatchResumeToken());
}

// A client's starting token may be a lower-version token whose eventIdentifier layout differs from
// the stream's own (higher-version) tokens. An endOfTransaction control event is always minted in
// the stream version, so the two tokens cannot be ordered by eventIdentifier value. Such an event
// must not be mistaken for one at or past the start point.
TEST_F(PlanExecutorTest, DoesNotAdvanceForVersionMismatchedControlEvent) {
    // A v1 token with no uuid may not carry an eventIdentifier, so its identifier is missing.
    ResumeTokenData startData{Timestamp(100, 1),
                              1,
                              kEndOfTransactionTxnOpIndex,
                              /*uuid=*/boost::none,
                              /*eventIdentifier=*/Value(),
                              ResumeTokenData::kNotFromInvalidate,
                              ResumeTokenData::kEventToken};
    const auto startToken = ResumeToken(startData);

    // The control event is minted in the stream version (2) and carries a wrapped identifier.
    ResumeTokenData eventData{Timestamp(100, 1),
                              ResumeTokenData::kDefaultTokenVersion,
                              kEndOfTransactionTxnOpIndex,
                              /*uuid=*/boost::none,
                              /*eventIdentifier=*/
                              Value(Document{{"operationType", 1}}),
                              ResumeTokenData::kNotFromInvalidate,
                              ResumeTokenData::kEventToken};
    const auto eventToken = ResumeToken(eventData);

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(eventToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(eventToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));
    // The event's identifier cannot be ordered against the lower-version start token, so the PBRT
    // stays at the start point.
    ASSERT_BSONOBJ_EQ(startToken.toBSON(), exec->getPostBatchResumeToken());
}

// A high-water-mark start token at the same clusterTime as an event token does not block the PBRT.
TEST_F(PlanExecutorTest, AdvancesForEventAtHighWaterMarkStartPoint) {
    const auto startToken = ResumeToken::makeHighWaterMarkToken(Timestamp(100, 1), 2);
    const auto eventToken = makeEventTokenV(Timestamp(100, 1), 2, UUID::gen());

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(eventToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(eventToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));
    ASSERT_BSONOBJ_EQ(eventToken.toBSON(), exec->getPostBatchResumeToken());
}

/**
 * A change stream on a shard must never advertise a post-batch resume token that sorts before the
 * stream's starting point. This happens when the router rebuilds the stream for an
 * end-of-transaction lookback: the oplog scan starts before the resume point and an accounting
 * control event below it passes through suppression.
 */
TEST_F(PlanExecutorTest, PbrtDoesNotRegressBelowStartPoint) {
    const auto startToken = makeEventToken(Timestamp(100, 1));
    const auto earlierToken = makeEventToken(Timestamp(50, 1));

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());
    enableBackfilling(_expCtx);

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(earlierToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(earlierToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));

    // The event's token sorts before the start point, so the PBRT stays at the start point.
    ASSERT_BSONOBJ_EQ(startToken.toBSON(), exec->getPostBatchResumeToken());
}

/**
 * A change stream event whose token is at or after the stream's starting point still advances the
 * post-batch resume token.
 */
TEST_F(PlanExecutorTest, PbrtAdvancesForEventAtOrAfterStartPoint) {
    const auto startToken = makeEventToken(Timestamp(50, 1));
    const auto laterToken = makeEventToken(Timestamp(100, 1));

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(laterToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(laterToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));

    // The event's token sorts after the start point, so the PBRT advances to it.
    ASSERT_BSONOBJ_EQ(laterToken.toBSON(), exec->getPostBatchResumeToken());
}

/**
 * Events are returned in resume-token order. Once one is at or after the start point the
 * start-point comparison is latched off; later events must still advance the post-batch resume
 * token, and an earlier event must not lower it.
 */
TEST_F(PlanExecutorTest, PbrtAdvancesAcrossMultipleEventsAfterStartPoint) {
    const auto startToken = makeEventToken(Timestamp(50, 1));
    const auto beforeToken = makeEventToken(Timestamp(40, 1));
    const auto firstAfterToken = makeEventToken(Timestamp(100, 1));
    const auto secondAfterToken = makeEventToken(Timestamp(200, 1));

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());
    enableBackfilling(_expCtx);

    auto makeAccountingEvent = [](const ResumeToken& token) {
        MutableDocument docBuilder;
        docBuilder.addField("_id", Value(token.toDocument()));
        docBuilder.addField("test", Value(true));
        docBuilder.metadata().setSortKey(Value(token.toDocument()), /*isSingleElementKey=*/true);
        docBuilder.metadata().setChangeStreamControlEvent(
            DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);
        return exec::agg::GetNextResult::makeAdvancedControlDocument(docBuilder.freeze());
    };

    std::deque<exec::agg::GetNextResult> results;
    results.push_back(makeAccountingEvent(beforeToken));
    results.push_back(makeAccountingEvent(firstAfterToken));
    results.push_back(makeAccountingEvent(secondAfterToken));

    auto mock = DocumentSourceMock::createForTest(std::move(results), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;

    // The event before the start point does not lower the PBRT.
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));
    ASSERT_BSONOBJ_EQ(startToken.toBSON(), exec->getPostBatchResumeToken());

    // The first event at or after the start point advances the PBRT.
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));
    ASSERT_BSONOBJ_EQ(firstAfterToken.toBSON(), exec->getPostBatchResumeToken());

    // The next event advances the PBRT even though the start-point comparison is now latched off.
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));
    ASSERT_BSONOBJ_EQ(secondAfterToken.toBSON(), exec->getPostBatchResumeToken());
}

/**
 * A stream can be resumed from a fragment of a split event. When the router rebuilds the stream for
 * an end-of-transaction lookback, an earlier fragment of the same event can be scanned again. Its
 * token has the same cluster time and transaction operation index as the start point but a lower
 * fragment number, so it sorts before it and must not lower the post-batch resume token.
 */
TEST_F(PlanExecutorTest, PbrtDoesNotRegressForLowerFragmentNum) {
    const UUID uuid = UUID::gen();
    const auto startToken = makeFragmentToken(Timestamp(100, 1), uuid, /*fragmentNum=*/1);
    const auto earlierToken = makeFragmentToken(Timestamp(100, 1), uuid, /*fragmentNum=*/0);

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(earlierToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(earlierToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));

    // The event's token sorts before the start point, so the PBRT stays at the start point.
    ASSERT_BSONOBJ_EQ(startToken.toBSON(), exec->getPostBatchResumeToken());
}

/**
 * A stream can be resumed from an invalidate notification. When the router rebuilds the stream for
 * an end-of-transaction lookback, the invalidating command that produced the notification can be
 * scanned again. Its token has the same cluster time and transaction operation index as the start
 * point but the invalidate flag cleared, so it sorts before it and must not lower the post-batch
 * resume token.
 */
TEST_F(PlanExecutorTest, PbrtDoesNotRegressForClearedInvalidateFlag) {
    const UUID uuid = UUID::gen();
    const auto startToken = makeInvalidateToken(Timestamp(100, 1), uuid, /*fromInvalidate=*/true);
    const auto earlierToken =
        makeInvalidateToken(Timestamp(100, 1), uuid, /*fromInvalidate=*/false);

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(earlierToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(earlierToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));

    // The event's token sorts before the start point, so the PBRT stays at the start point.
    ASSERT_BSONOBJ_EQ(startToken.toBSON(), exec->getPostBatchResumeToken());
}

/**
 * A stream resumed from a fragment of a split event starts from the client's fragment. The unsplit
 * event that carries the fragment sorts before the client's fragment token, so it must not lower
 * the post-batch resume token; the split fragments advance it instead.
 */
TEST_F(PlanExecutorTest, PbrtDoesNotRegressForUnsplitEventWhenResumingFromFragment) {
    const UUID uuid = UUID::gen();
    const auto startToken = makeFragmentToken(Timestamp(100, 1), uuid, /*fragmentNum=*/1);
    const auto unsplitToken = makeEventTokenV(Timestamp(100, 1), 2, uuid);

    _expCtx->setForPerShardCursor(true);
    _expCtx->setInitialPostBatchResumeToken(startToken.toBSON());

    MutableDocument docBuilder;
    docBuilder.addField("_id", Value(unsplitToken.toDocument()));
    docBuilder.addField("test", Value(true));
    docBuilder.metadata().setSortKey(Value(unsplitToken.toDocument()),
                                     /*isSingleElementKey=*/true);
    docBuilder.metadata().setChangeStreamControlEvent(
        DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

    auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
    auto pipeline = Pipeline::create({mock}, _expCtx);
    auto exec = plan_executor_factory::make(
        _expCtx, std::move(pipeline), PlanExecutorPipeline::ResumableScanType::kChangeStream);

    BSONObj objOut;
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, exec->getNext(&objOut, nullptr));

    // The unsplit event's token sorts before the start point, so the PBRT stays at the start point.
    ASSERT_BSONOBJ_EQ(startToken.toBSON(), exec->getPostBatchResumeToken());
}

TEST_F(PlanExecutorTest, ClassicPipelineDisposalExcludesCleanupAndAccountsForPriorWork) {
    unittest::ServerParameterGuard interval("overdueInterruptCheckIntervalMillis", 10);
    auto* ticks = _serviceContext.tickSource();
    auto source = make_intrusive<TimedDisposeDocumentSource>(_expCtx);
    auto pipeline = Pipeline::create({source}, _expCtx);
    auto exec = plan_executor_factory::make(_expCtx, std::move(pipeline));
    ASSERT(dynamic_cast<PlanExecutorPipeline*>(exec.get()));

    _opCtx->trackOverdueInterruptChecks(ticks->getTicks());
    const auto* stats = _opCtx->overdueInterruptCheckStats();
    ticks->advance(Milliseconds{20});
    exec->dispose(_opCtx.get());

    ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
    ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{10});
    _opCtx->updateInterruptCheckCounters();
    ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
    ticks->advance(Milliseconds{20});
    _opCtx->updateInterruptCheckCounters();
    ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 2);
    ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{20});
}

}  // namespace
}  // namespace mongo
