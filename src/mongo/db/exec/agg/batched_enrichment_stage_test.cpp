// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/batched_enrichment_stage.h"

#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <functional>
#include <string>
#include <vector>

namespace mongo::exec::agg {
namespace {

using Limits = BatchedEnrichmentStage::Limits;

// Concrete BatchedEnrichmentStage used to drive the base loop in isolation: records the
// begin/close hook calls, asserts the enrich() contract (called only while a scope is open), and
// applies a configurable per-event transform.
class EnrichmentStageMock : public BatchedEnrichmentStage {
public:
    EnrichmentStageMock(const boost::intrusive_ptr<ExpressionContext>& expCtx, Limits limits)
        : BatchedEnrichmentStage("$mockEnrich", expCtx, limits) {}

    // The transform applied to each data event in enrich(). Defaults to tagging it 'enriched:
    // true'.
    std::function<Document(Document)> enrichFn = [](Document d) {
        MutableDocument md(std::move(d));
        md.addField("enriched", Value(true));
        return md.freeze();
    };

    int beginCalls = 0;
    int closeCalls = 0;
    int enrichCalls = 0;
    bool scopeOpen = false;

protected:
    void beginBatch() override {
        // Only one scope is ever open at a time; this guards that invariant directly.
        ASSERT_FALSE(scopeOpen) << "beginBatch() called while a scope was already open";
        scopeOpen = true;
        ++beginCalls;
    }
    void closeBatch() noexcept override {
        scopeOpen = false;
        ++closeCalls;
    }
    Document enrich(Document event) override {
        ASSERT_TRUE(scopeOpen) << "enrich() called outside an open scope";
        ++enrichCalls;
        return enrichFn(std::move(event));
    }
};

// A source stage that yields a fixed sequence of data events and then throws (instead of ever
// reaching EOF), simulating ChangeStreamCheckInvalidateStage's queue-an-event-then-throw protocol.
class ThrowingSourceStage : public Stage {
public:
    ThrowingSourceStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        std::vector<Document> docs,
                        std::function<void()> throwFn)
        : Stage("$mockThrowingSource", expCtx),
          _docs(std::move(docs)),
          _throwFn(std::move(throwFn)) {}

protected:
    GetNextResult doGetNext() override {
        if (_index < _docs.size()) {
            return GetNextResult(Document(_docs[_index++]));
        }
        _throwFn();
        MONGO_UNREACHABLE;
    }

private:
    std::vector<Document> _docs;
    size_t _index = 0;
    std::function<void()> _throwFn;
};

class BatchedEnrichmentStageTest : public unittest::Test {
protected:
    boost::intrusive_ptr<EnrichmentStageMock> makeStage(boost::intrusive_ptr<MockStage> source,
                                                        Limits limits) {
        auto stage = make_intrusive<EnrichmentStageMock>(_expCtx, limits);
        MockStage::setSource_forTest(stage, source.get());
        _source = std::move(source);  // keep the source alive for the stage's lifetime
        return stage;
    }

    // A generous, never-tripped set of caps unless a test overrides one.
    Limits looseLimits() const {
        return Limits{
            .maxInputEvents = 1000, .maxInputBytes = 1024 * 1024, .maxOutputBytes = 1024 * 1024};
    }

    static Document dataEvent(int id) {
        return Document{{"_id", id}};
    }

    static Document controlEvent(int id) {
        MutableDocument md;
        md.addField("_id", Value(id));
        md.metadata().setChangeStreamControlEvent();
        return md.freeze();
    }

    // Drains the stage to EOF, returning the emitted results in order. Asserts no scope is left
    // open and the begin/close hooks are balanced at the end.
    std::vector<GetNextResult> drainToEOF(EnrichmentStageMock& stage) {
        std::vector<GetNextResult> out;
        while (true) {
            auto next = stage.getNext();
            if (next.isEOF()) {
                break;
            }
            ASSERT_TRUE(next.isAdvanced() || next.isAdvancedControlDocument());
            out.push_back(std::move(next));
        }
        ASSERT_FALSE(stage.scopeOpen);
        ASSERT_EQ(stage.beginCalls, stage.closeCalls);
        return out;
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx = make_intrusive<ExpressionContextForTest>();
    boost::intrusive_ptr<MockStage> _source;
};

using BatchedEnrichmentStageDeathTest = BatchedEnrichmentStageTest;

DEATH_TEST_REGEX_F(BatchedEnrichmentStageDeathTest, ZeroMaxInputEventsTasserts, "12916800") {
    auto limits = looseLimits();
    limits.maxInputEvents = 0;
    make_intrusive<EnrichmentStageMock>(_expCtx, limits);
}

DEATH_TEST_REGEX_F(BatchedEnrichmentStageDeathTest, ZeroMaxInputBytesTasserts, "12916801") {
    auto limits = looseLimits();
    limits.maxInputBytes = 0;
    make_intrusive<EnrichmentStageMock>(_expCtx, limits);
}

DEATH_TEST_REGEX_F(BatchedEnrichmentStageDeathTest, ZeroMaxOutputBytesTasserts, "12916802") {
    auto limits = looseLimits();
    limits.maxOutputBytes = 0;
    make_intrusive<EnrichmentStageMock>(_expCtx, limits);
}

TEST_F(BatchedEnrichmentStageTest, EnrichesAllEventsInArrivalOrderInOneScope) {
    auto source = MockStage::createForTest(
        std::vector<Document>{dataEvent(1), dataEvent(2), dataEvent(3)}, _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    auto out = drainToEOF(*stage);

    ASSERT_EQ(out.size(), 3u);
    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(out[i].getDocument()["_id"].getInt(), static_cast<int>(i) + 1);
        ASSERT_TRUE(out[i].getDocument()["enriched"].getBool());
    }
    // All three enriched under a single scope, opened and closed once.
    ASSERT_EQ(stage->beginCalls, 1);
    ASSERT_EQ(stage->closeCalls, 1);
    ASSERT_EQ(stage->enrichCalls, 3);
}

TEST_F(BatchedEnrichmentStageTest, MaxEventsOfOneEnrichesEachEventInItsOwnScope) {
    auto source = MockStage::createForTest(
        std::vector<Document>{dataEvent(1), dataEvent(2), dataEvent(3)}, _expCtx);
    auto limits = looseLimits();
    limits.maxInputEvents = 1;
    auto stage = makeStage(std::move(source), limits);

    auto out = drainToEOF(*stage);

    ASSERT_EQ(out.size(), 3u);
    // Batch-of-1: one fill -> one enrich scope -> one drain, per event.
    ASSERT_EQ(stage->beginCalls, 3);
    ASSERT_EQ(stage->closeCalls, 3);
    ASSERT_EQ(stage->enrichCalls, 3);
}

TEST_F(BatchedEnrichmentStageTest, MaxInputBytesStopsFillButAlwaysAdmitsAtLeastOneEvent) {
    auto source = MockStage::createForTest(
        std::vector<Document>{dataEvent(1), dataEvent(2), dataEvent(3), dataEvent(4)}, _expCtx);
    auto limits = looseLimits();
    // A byte budget far below a single event's size forces one event per fill (batch-of-1 by
    // bytes).
    limits.maxInputBytes = 1;
    auto stage = makeStage(std::move(source), limits);

    auto out = drainToEOF(*stage);

    ASSERT_EQ(out.size(), 4u);
    // Each event is admitted alone (>= 1 guarantee), so four separate scopes.
    ASSERT_EQ(stage->beginCalls, 4);
    ASSERT_EQ(stage->closeCalls, 4);
    ASSERT_EQ(stage->enrichCalls, 4);
}

TEST_F(BatchedEnrichmentStageTest, MaxOutputBytesSuspendsMidInputAcrossMultipleScopes) {
    auto source = MockStage::createForTest(
        std::vector<Document>{dataEvent(1), dataEvent(2), dataEvent(3)}, _expCtx);
    auto limits = looseLimits();
    // One enriched event easily exceeds this, so the scope suspends after each event.
    limits.maxOutputBytes = 1;
    auto stage = makeStage(std::move(source), limits);

    auto out = drainToEOF(*stage);

    // All events still emitted in order despite suspending mid-input.
    ASSERT_EQ(out.size(), 3u);
    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(out[i].getDocument()["_id"].getInt(), static_cast<int>(i) + 1);
    }
    // One fill batch of three, but the output cap forces three separate enrich scopes.
    ASSERT_EQ(stage->beginCalls, 3);
    ASSERT_EQ(stage->closeCalls, 3);
    ASSERT_EQ(stage->enrichCalls, 3);
}

TEST_F(BatchedEnrichmentStageTest, OversizedSingleEventIsEnrichedAsABatchOfOne) {
    auto source = MockStage::createForTest(std::vector<Document>{dataEvent(1)}, _expCtx);
    auto limits = looseLimits();
    limits.maxInputBytes = 1;
    limits.maxOutputBytes = 1;
    auto stage = makeStage(std::move(source), limits);

    auto out = drainToEOF(*stage);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].getDocument()["enriched"].getBool());
    ASSERT_EQ(stage->beginCalls, 1);
    ASSERT_EQ(stage->closeCalls, 1);
    ASSERT_EQ(stage->enrichCalls, 1);
}

TEST_F(BatchedEnrichmentStageTest, ControlEventsPassThroughInOrderWithoutEnteringEnrich) {
    std::deque<GetNextResult> results;
    results.emplace_back(dataEvent(1));
    results.emplace_back(GetNextResult::makeAdvancedControlDocument(controlEvent(2)));
    results.emplace_back(dataEvent(3));
    auto source = MockStage::createForTest(std::move(results), _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    auto out = drainToEOF(*stage);

    ASSERT_EQ(out.size(), 3u);
    // Data event: enriched and advanced.
    ASSERT_TRUE(out[0].isAdvanced());
    ASSERT_TRUE(out[0].getDocument()["enriched"].getBool());
    // Control event: passed through untouched, still a control document, not enriched.
    ASSERT_TRUE(out[1].isAdvancedControlDocument());
    ASSERT_TRUE(out[1].getDocument().metadata().isChangeStreamControlEvent());
    ASSERT_TRUE(out[1].getDocument()["enriched"].missing());
    // Third data event: enriched.
    ASSERT_TRUE(out[2].isAdvanced());
    ASSERT_TRUE(out[2].getDocument()["enriched"].getBool());
    // Two windows ([d1] then [d3], split by the control boundary); only the data events enriched.
    ASSERT_EQ(stage->beginCalls, 2);
    ASSERT_EQ(stage->closeCalls, 2);
    ASSERT_EQ(stage->enrichCalls, 2);
}

TEST_F(BatchedEnrichmentStageTest, FillStopsAtControlEventBoundary) {
    // A control event ends the current fill: events after it are not buffered into the same window,
    // so the control acts as a batch boundary even when no cap trips. With caps loose, [d1, c2]
    // form the first window and [d3, d4] the second -> two enrich scopes (one window without the
    // boundary).
    std::deque<GetNextResult> results;
    results.emplace_back(dataEvent(1));
    results.emplace_back(GetNextResult::makeAdvancedControlDocument(controlEvent(2)));
    results.emplace_back(dataEvent(3));
    results.emplace_back(dataEvent(4));
    auto source = MockStage::createForTest(std::move(results), _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    auto out = drainToEOF(*stage);

    ASSERT_EQ(out.size(), 4u);
    ASSERT_EQ(out[0].getDocument()["_id"].getInt(), 1);
    ASSERT_TRUE(out[1].isAdvancedControlDocument());
    ASSERT_EQ(out[2].getDocument()["_id"].getInt(), 3);
    ASSERT_EQ(out[3].getDocument()["_id"].getInt(), 4);
    // The control event forced a boundary: window [d1] then window [d3, d4] (c2 is the boundary,
    // not enriched), so two scopes and three enriched events.
    ASSERT_EQ(stage->beginCalls, 2);
    ASSERT_EQ(stage->closeCalls, 2);
    ASSERT_EQ(stage->enrichCalls, 3);
}

TEST_F(BatchedEnrichmentStageTest, ControlEventMidWindowSurfacesOnlyAfterBufferedDataDrains) {
    // Two data events are buffered and enriched before a control event arrives mid-stream. The
    // control must surface in arrival order, only after both enriched data events have drained, and
    // the data event after it is enriched in a fresh window.
    std::deque<GetNextResult> results;
    results.emplace_back(dataEvent(1));
    results.emplace_back(dataEvent(2));
    results.emplace_back(GetNextResult::makeAdvancedControlDocument(controlEvent(3)));
    results.emplace_back(dataEvent(4));
    auto source = MockStage::createForTest(std::move(results), _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    auto out = drainToEOF(*stage);

    ASSERT_EQ(out.size(), 4u);
    ASSERT_TRUE(out[0].isAdvanced());
    ASSERT_EQ(out[0].getDocument()["_id"].getInt(), 1);
    ASSERT_TRUE(out[0].getDocument()["enriched"].getBool());
    ASSERT_TRUE(out[1].isAdvanced());
    ASSERT_EQ(out[1].getDocument()["_id"].getInt(), 2);
    ASSERT_TRUE(out[1].getDocument()["enriched"].getBool());
    // Control surfaces third, untouched, only after the two buffered data events drained.
    ASSERT_TRUE(out[2].isAdvancedControlDocument());
    ASSERT_EQ(out[2].getDocument()["_id"].getInt(), 3);
    ASSERT_TRUE(out[2].getDocument()["enriched"].missing());
    ASSERT_TRUE(out[3].isAdvanced());
    ASSERT_EQ(out[3].getDocument()["_id"].getInt(), 4);
    ASSERT_TRUE(out[3].getDocument()["enriched"].getBool());
    // Two windows: [d1, d2] before the control boundary, then [d4]. Control never enriched.
    ASSERT_EQ(stage->beginCalls, 2);
    ASSERT_EQ(stage->closeCalls, 2);
    ASSERT_EQ(stage->enrichCalls, 3);
}

TEST_F(BatchedEnrichmentStageTest, PauseMidWindowSurfacesOnlyAfterBufferedDataDrains) {
    // A pause arriving mid-stream shares the non-advanced code path with control events: two data
    // events are buffered and enriched, and the pause surfaces in arrival order only after both
    // drain, with the event after it enriched in a fresh window.
    std::deque<GetNextResult> results;
    results.emplace_back(dataEvent(1));
    results.emplace_back(dataEvent(2));
    results.push_back(GetNextResult::makePauseExecution());
    results.emplace_back(dataEvent(4));
    auto source = MockStage::createForTest(std::move(results), _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    auto e1 = stage->getNext();
    ASSERT_TRUE(e1.isAdvanced());
    ASSERT_EQ(e1.getDocument()["_id"].getInt(), 1);
    ASSERT_TRUE(e1.getDocument()["enriched"].getBool());

    auto e2 = stage->getNext();
    ASSERT_TRUE(e2.isAdvanced());
    ASSERT_EQ(e2.getDocument()["_id"].getInt(), 2);

    // Pause surfaces only after the two buffered data events drained.
    ASSERT_TRUE(stage->getNext().isPaused());

    auto e4 = stage->getNext();
    ASSERT_TRUE(e4.isAdvanced());
    ASSERT_EQ(e4.getDocument()["_id"].getInt(), 4);
    ASSERT_TRUE(e4.getDocument()["enriched"].getBool());

    ASSERT_TRUE(stage->getNext().isEOF());
    // Two windows: [d1, d2] before the pause boundary, then [d4]. Pause never enriched.
    ASSERT_EQ(stage->beginCalls, 2);
    ASSERT_EQ(stage->closeCalls, 2);
    ASSERT_EQ(stage->enrichCalls, 3);
    ASSERT_FALSE(stage->scopeOpen);
}

TEST_F(BatchedEnrichmentStageTest, EmptyBufferPropagatesPauseUntouched) {
    std::deque<GetNextResult> results;
    results.push_back(GetNextResult::makePauseExecution());
    results.emplace_back(dataEvent(1));
    auto source = MockStage::createForTest(std::move(results), _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    // First call: nothing buffered, upstream pauses -> propagate pause untouched, no scope opened.
    auto first = stage->getNext();
    ASSERT_TRUE(first.isPaused());
    ASSERT_EQ(stage->beginCalls, 0);
    ASSERT_EQ(stage->closeCalls, 0);
    ASSERT_EQ(stage->enrichCalls, 0);

    // Subsequent calls drain the now-available event.
    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(second.getDocument()["_id"].getInt(), 1);
}

TEST_F(BatchedEnrichmentStageTest, EmptyBufferPropagatesEOF) {
    auto source = MockStage::createForTest(std::deque<GetNextResult>{}, _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_EQ(stage->beginCalls, 0);
    ASSERT_EQ(stage->closeCalls, 0);
    ASSERT_EQ(stage->enrichCalls, 0);
}

TEST_F(BatchedEnrichmentStageTest, EnrichThrowClosesScopeAndPropagates) {
    auto source =
        MockStage::createForTest(std::vector<Document>{dataEvent(1), dataEvent(2)}, _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());
    stage->enrichFn = [](Document) -> Document {
        uasserted(ErrorCodes::InternalError, "simulated enrich failure");
    };

    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::InternalError);
    // The ScopeGuard ran closeBatch() on the throwing path, so no scope is left open.
    ASSERT_FALSE(stage->scopeOpen);
    ASSERT_EQ(stage->beginCalls, 1);
    ASSERT_EQ(stage->closeCalls, 1);
    // The throw originated in enrich() (not beginBatch() or elsewhere): exactly one enrich attempt.
    ASSERT_EQ(stage->enrichCalls, 1);
}

TEST_F(BatchedEnrichmentStageTest, BufferedMemoryIsTrackedAndReturnsToZeroWhenDrained) {
    auto source = MockStage::createForTest(
        std::vector<Document>{dataEvent(1), dataEvent(2), dataEvent(3)}, _expCtx);
    auto stage = makeStage(std::move(source), looseLimits());

    ASSERT_EQ(stage->bufferedMemoryBytes_forTest(), 0);

    // The first getNext() fills and enriches all three under the loose caps and emits one; the
    // other two enriched events stay buffered, so tracked memory must be positive mid-flight.
    // Sampling here (not just at the ends) catches a trackPush/trackPop sign error that would still
    // net to zero across a full drain.
    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    const int64_t inFlightBytes = stage->bufferedMemoryBytes_forTest();
    ASSERT_GT(inFlightBytes, 0);

    int emitted = 1;
    while (true) {
        auto next = stage->getNext();
        if (next.isEOF()) {
            break;
        }
        ASSERT_TRUE(next.isAdvanced());
        ++emitted;
    }
    ASSERT_EQ(emitted, 3);

    // Fully drained: nothing remains buffered, and the peak covered the mid-flight high-water mark.
    ASSERT_EQ(stage->bufferedMemoryBytes_forTest(), 0);
    ASSERT_GE(stage->peakBufferedMemoryBytes_forTest(), inFlightBytes);
    // One window enriched all three, opened and closed once.
    ASSERT_EQ(stage->beginCalls, 1);
    ASSERT_EQ(stage->closeCalls, 1);
    ASSERT_EQ(stage->enrichCalls, 3);
}

TEST_F(BatchedEnrichmentStageTest,
       PendingChangeStreamInvalidatedDrainsBufferedEventsBeforeRethrow) {
    // Mirrors ChangeStreamCheckInvalidateStage's real protocol: it returns an "invalidate" event
    // (here, just another data event as far as this stage cares) on one call, then throws
    // ChangeStreamInvalidated on the next. fillBatch() must not lose the two already-buffered
    // events underneath that throw; they must still reach the caller, enriched, before the
    // exception surfaces.
    auto invalidateToken = BSON("_data" << "some-invalidate-token");
    auto source = make_intrusive<ThrowingSourceStage>(
        _expCtx, std::vector<Document>{dataEvent(1), dataEvent(2)}, [invalidateToken] {
            uasserted(ChangeStreamInvalidationInfo(invalidateToken),
                      "simulated change stream invalidate");
        });
    auto stage = make_intrusive<EnrichmentStageMock>(_expCtx, looseLimits());
    MockStage::setSource_forTest(stage, source.get());

    auto e1 = stage->getNext();
    ASSERT_TRUE(e1.isAdvanced());
    ASSERT_EQ(e1.getDocument()["_id"].getInt(), 1);
    ASSERT_TRUE(e1.getDocument()["enriched"].getBool());

    auto e2 = stage->getNext();
    ASSERT_TRUE(e2.isAdvanced());
    ASSERT_EQ(e2.getDocument()["_id"].getInt(), 2);
    ASSERT_TRUE(e2.getDocument()["enriched"].getBool());

    // Only once both buffered events have drained does the exception surface, with its extra info
    // (the resume token getmore_cmd.cpp needs) intact.
    try {
        stage->getNext();
        FAIL("expected ChangeStreamInvalidated to be thrown");
    } catch (const ExceptionFor<ErrorCodes::ChangeStreamInvalidated>& ex) {
        auto extraInfo = ex.extraInfo<ChangeStreamInvalidationInfo>();
        ASSERT_TRUE(extraInfo);
        ASSERT_BSONOBJ_EQ(extraInfo->getInvalidateResumeToken(), invalidateToken);
    }

    ASSERT_FALSE(stage->scopeOpen);
    ASSERT_EQ(stage->beginCalls, 1);
    ASSERT_EQ(stage->closeCalls, 1);
    ASSERT_EQ(stage->enrichCalls, 2);
}

TEST_F(BatchedEnrichmentStageTest, PendingCloseChangeStreamDrainsBufferedEventsBeforeRethrow) {
    // Same hazard, different code: CloseChangeStream carries no extra info, but must still let
    // already-buffered events drain first.
    auto source =
        make_intrusive<ThrowingSourceStage>(_expCtx, std::vector<Document>{dataEvent(1)}, [] {
            uasserted(ErrorCodes::CloseChangeStream, "simulated close change stream");
        });
    auto stage = make_intrusive<EnrichmentStageMock>(_expCtx, looseLimits());
    MockStage::setSource_forTest(stage, source.get());

    auto e1 = stage->getNext();
    ASSERT_TRUE(e1.isAdvanced());
    ASSERT_EQ(e1.getDocument()["_id"].getInt(), 1);
    ASSERT_TRUE(e1.getDocument()["enriched"].getBool());

    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::CloseChangeStream);
    ASSERT_FALSE(stage->scopeOpen);
    ASSERT_EQ(stage->beginCalls, 1);
    ASSERT_EQ(stage->closeCalls, 1);
    ASSERT_EQ(stage->enrichCalls, 1);
}

TEST_F(BatchedEnrichmentStageTest, PendingExceptionWithNothingBufferedRethrowsImmediately) {
    // Edge case: the exception fires on the very first upstream pull, with nothing buffered ahead
    // of it. No scope should ever be opened.
    auto source = make_intrusive<ThrowingSourceStage>(_expCtx, std::vector<Document>{}, [] {
        uasserted(ErrorCodes::CloseChangeStream, "simulated close change stream");
    });
    auto stage = make_intrusive<EnrichmentStageMock>(_expCtx, looseLimits());
    MockStage::setSource_forTest(stage, source.get());

    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::CloseChangeStream);
    ASSERT_FALSE(stage->scopeOpen);
    ASSERT_EQ(stage->beginCalls, 0);
    ASSERT_EQ(stage->closeCalls, 0);
    ASSERT_EQ(stage->enrichCalls, 0);
}

TEST_F(BatchedEnrichmentStageTest, InterruptIsCheckedAndPropagates) {
    // ExpressionContext::checkForInterrupt() throttles to an actual opCtx check every 128 calls, so
    // process enough events that the periodic check fires (whether during fill's upstream pulls or
    // the per-iteration check in the enrich loop) and the interrupt propagates as a throw. No scope
    // is leaked regardless of where it fires (the close-on-throw path is covered separately by
    // EnrichThrowClosesScope).
    constexpr int kNumEvents = 300;
    std::vector<Document> docs;
    docs.reserve(kNumEvents);
    for (int i = 0; i < kNumEvents; ++i) {
        docs.push_back(dataEvent(i));
    }
    auto limits = looseLimits();
    limits.maxInputEvents = kNumEvents + 1;
    auto stage = makeStage(MockStage::createForTest(docs, _expCtx), limits);

    _expCtx->getOperationContext()->markKilled(ErrorCodes::Interrupted);
    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::Interrupted);
    // Wherever the interrupt fired, no scope is left open and any opened scope was closed (the
    // exact counts depend on interrupt timing, but begin/close must balance).
    ASSERT_FALSE(stage->scopeOpen);
    ASSERT_EQ(stage->beginCalls, stage->closeCalls);
}

}  // namespace
}  // namespace mongo::exec::agg
