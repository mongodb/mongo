// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/change_stream_check_resumability_stage.h"
#include "mongo/db/exec/agg/change_stream_convert_errors_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/uuid.h"

#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// The base fixture for the CheckResumability error-as-events tests: an aggregation context plus a
// stable test uuid.
class CheckResumabilityErrorAsEventsTest : public AggregationContextFixture {
protected:
    static const UUID& testUuid() {
        static const UUID* uuid = new UUID(UUID::gen());
        return *uuid;
    }
};

// A source which returns queued results, or throws the queued status, in order. Used to simulate
// errors raised by the oplog scan.
class ThrowingSourceStage : public exec::agg::Stage {
public:
    explicit ThrowingSourceStage(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : exec::agg::Stage("$throwingSource"sv, expCtx) {}

    void push_back(exec::agg::GetNextResult result) {
        _queue.emplace_back(std::move(result));
    }

    void pushError(Status status) {
        _queue.emplace_back(std::move(status));
    }

protected:
    exec::agg::GetNextResult doGetNext() override {
        if (_queue.empty()) {
            return exec::agg::GetNextResult::makeEOF();
        }
        auto next = std::move(_queue.front());
        _queue.pop_front();
        if (auto* status = std::get_if<Status>(&next)) {
            uassertStatusOK(*status);
        }
        return std::get<exec::agg::GetNextResult>(std::move(next));
    }

private:
    std::deque<std::variant<exec::agg::GetNextResult, Status>> _queue;
};

class CheckResumabilityTranslationTest : public CheckResumabilityErrorAsEventsTest {
protected:
    static constexpr std::string_view kCappedPositionLostMsg = "oplog rolled over"sv;

    void setShowErrorsAsEvents(bool showErrorsAsEvents) {
        DocumentSourceChangeStreamSpec spec;
        spec.setShowErrorsAsEvents(showErrorsAsEvents);
        getExpCtx()->setChangeStreamSpec(spec);
    }

    ResumeTokenData makeToken(std::string_view id, int version = 0) const {
        return {Timestamp(100, 1), version, 0, testUuid(), Value(Document{{"_id"sv, id}})};
    }

    Document makeEvent(std::string_view id) const {
        MutableDocument doc;
        const auto tokenDoc = ResumeToken(makeToken(id)).toDocument();
        doc.setField("_id"sv, Value(tokenDoc));
        doc.metadata().setSortKey(Value(tokenDoc), true);
        return doc.freeze();
    }

    // An operationType:"error" event with the token for 'id' as _id and sort key, or without a
    // sort key.
    Document makeErrorEvent(std::string_view id, bool withSortKey = true, int version = 0) const {
        MutableDocument doc;
        const auto tokenDoc = ResumeToken(makeToken(id, version)).toDocument();
        doc.setField("_id"sv, Value(tokenDoc));
        doc.setField("operationType"sv, Value("error"sv));
        doc.setField("error"sv, Value(Document{{"source"sv, "user-pipeline"sv}}));
        if (withSortKey) {
            doc.metadata().setSortKey(Value(tokenDoc), true);
        }
        return doc.freeze();
    }

    // Builds a stage resuming from the token with _id "2", fed by a source which has the queued
    // results given by 'setUpSource'.
    intrusive_ptr<exec::agg::ChangeStreamCheckResumabilityStage> makeStage() {
        auto token = makeToken("2");
        DocumentSourceChangeStreamSpec spec;
        spec.setStartAfter(ResumeToken(token));
        auto ds = DocumentSourceChangeStreamCheckResumability::create(getExpCtx(), spec);
        auto stage = boost::dynamic_pointer_cast<exec::agg::ChangeStreamCheckResumabilityStage>(
            exec::agg::buildStage(ds));
        _source = make_intrusive<ThrowingSourceStage>(getExpCtx());
        exec::agg::MockStage::setSource_forTest(stage, _source.get());
        return stage;
    }

    void assertErrorCode(ErrorCodes::Error expected,
                         intrusive_ptr<exec::agg::ChangeStreamCheckResumabilityStage> stage) {
        try {
            stage->getNext();
            FAIL("Expected the stage to throw");
        } catch (const DBException& ex) {
            ASSERT_EQ(ex.code(), expected);
        }
    }

    intrusive_ptr<ThrowingSourceStage> _source;
};

TEST_F(CheckResumabilityTranslationTest, TranslationCappedPositionLostBeforeResumePointOptedIn) {
    setShowErrorsAsEvents(true);
    auto stage = makeStage();
    _source->push_back(makeEvent("1"));
    _source->pushError({ErrorCodes::CappedPositionLost, kCappedPositionLostMsg});
    assertErrorCode(ErrorCodes::RetryChangeStream, stage);
}

TEST_F(CheckResumabilityTranslationTest, TranslationCappedPositionLostLogsDebugMessage) {
    unittest::MinimumLoggedSeverityGuard severityGuard{logv2::LogComponent::kQuery,
                                                       logv2::LogSeverity::Debug(3)};
    setShowErrorsAsEvents(true);
    auto stage = makeStage();
    _source->pushError({ErrorCodes::CappedPositionLost, kCappedPositionLostMsg});

    unittest::LogCaptureGuard logs;
    assertErrorCode(ErrorCodes::RetryChangeStream, stage);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 13732400)), 1);
}

TEST_F(CheckResumabilityTranslationTest, TranslationCappedPositionLostBeforeResumePointNotOptedIn) {
    setShowErrorsAsEvents(false);
    auto stage = makeStage();
    _source->push_back(makeEvent("1"));
    _source->pushError({ErrorCodes::CappedPositionLost, kCappedPositionLostMsg});
    assertErrorCode(ErrorCodes::CappedPositionLost, stage);
}

TEST_F(CheckResumabilityTranslationTest, TranslationCappedPositionLostAfterResumePointOptedIn) {
    setShowErrorsAsEvents(true);
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));
    _source->pushError({ErrorCodes::CappedPositionLost, kCappedPositionLostMsg});
    ASSERT_TRUE(stage->getNext().isAdvanced());
    assertErrorCode(ErrorCodes::RetryChangeStream, stage);
}

TEST_F(CheckResumabilityTranslationTest, TranslationCappedPositionLostAfterResumePointNotOptedIn) {
    setShowErrorsAsEvents(false);
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));
    _source->pushError({ErrorCodes::CappedPositionLost, kCappedPositionLostMsg});
    ASSERT_TRUE(stage->getNext().isAdvanced());
    assertErrorCode(ErrorCodes::CappedPositionLost, stage);
}

TEST_F(CheckResumabilityTranslationTest, TranslationMinTsMissingBeforeResumePoint) {
    for (bool optedIn : {true, false}) {
        setShowErrorsAsEvents(optedIn);
        auto stage = makeStage();
        _source->pushError({ErrorCodes::OplogQueryMinTsMissing, "fell off"});
        assertErrorCode(ErrorCodes::ChangeStreamHistoryLost, stage);
    }
}

TEST_F(CheckResumabilityTranslationTest, TranslationMinTsMissingAfterResumePointPropagates) {
    for (bool optedIn : {true, false}) {
        setShowErrorsAsEvents(optedIn);
        auto stage = makeStage();
        _source->push_back(makeEvent("3"));
        _source->pushError({ErrorCodes::OplogQueryMinTsMissing, "fell off"});
        ASSERT_TRUE(stage->getNext().isAdvanced());
        assertErrorCode(ErrorCodes::OplogQueryMinTsMissing, stage);
    }
}

TEST_F(CheckResumabilityTranslationTest, TranslationLeavesOtherErrorsUnchanged) {
    setShowErrorsAsEvents(true);
    auto stage = makeStage();
    _source->pushError({ErrorCodes::InternalError, "other"});
    assertErrorCode(ErrorCodes::InternalError, stage);
}

/**
 * A process interface which reports that the node is a shard with the given id.
 */
class ShardIdProcessInterface final : public StubMongoProcessInterface {
public:
    explicit ShardIdProcessInterface(boost::optional<ShardId> shardId)
        : _shardId(std::move(shardId)) {}

    boost::optional<ShardId> getShardId(OperationContext*) const override {
        return _shardId;
    }

private:
    boost::optional<ShardId> _shardId;
};

class CheckResumabilityGapBuildTest : public CheckResumabilityTranslationTest {
protected:
    void setUp() override {
        CheckResumabilityTranslationTest::setUp();
        setShowErrorsAsEvents(true);
        getExpCtx()->setChangeStreamOplogFloorAfterRollover(kFloor);
        setShardId(boost::none);
    }

    void setShardId(boost::optional<ShardId> shardId) {
        getExpCtx()->setMongoProcessInterface(
            std::make_shared<ShardIdProcessInterface>(std::move(shardId)));
    }

    // Returns the code of the exception thrown by the stage's next getNext().
    ErrorCodes::Error getSignalCode(
        const intrusive_ptr<exec::agg::ChangeStreamCheckResumabilityStage>& stage) {
        try {
            stage->getNext();
        } catch (const DBException& ex) {
            return ex.code();
        }
        FAIL("Expected the stage to throw");
        MONGO_UNREACHABLE;
    }

    static inline const Timestamp kFloor{200, 1};
};

TEST_F(CheckResumabilityGapBuildTest, GapBuildLogsDebugMessage) {
    unittest::MinimumLoggedSeverityGuard severityGuard{logv2::LogComponent::kQuery,
                                                       logv2::LogSeverity::Debug(3)};
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));

    unittest::LogCaptureGuard logs;
    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 13732401)), 1);
}

TEST_F(CheckResumabilityGapBuildTest, GapBuildStashesGapAndSignals) {
    auto stage = makeStage();
    _source->push_back(makeEvent("1"));
    _source->push_back(makeEvent("3"));

    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);

    const auto& gap = getExpCtx()->getPendingChangeStreamResumeGap();
    ASSERT_TRUE(gap.has_value());
    ASSERT_DOCUMENT_EQ((*gap)["_id"].getDocument(), ResumeToken(makeToken("2")).toDocument());
    ASSERT_EQ((*gap)["operationType"].getStringData(), "error"sv);
    ASSERT_EQ((*gap)["clusterTime"].getTimestamp(), makeToken("2").clusterTime);
    const auto error = (*gap)["error"].getDocument();
    ASSERT_EQ(error["source"].getStringData(), "resume-gap"sv);
    ASSERT_EQ(error["code"].getInt(), ErrorCodes::ChangeStreamHistoryLost);
    ASSERT_EQ(error["earliestOplogTimestamp"].getTimestamp(), kFloor);
    ASSERT_EQ(error["skippedClusterTimeRange"]["from"].getTimestamp(), makeToken("2").clusterTime);
    ASSERT_EQ(error["skippedClusterTimeRange"]["to"].getTimestamp(), kFloor);
    ASSERT_TRUE(error["shardId"].missing());
}

TEST_F(CheckResumabilityGapBuildTest, GapBuildOneGapPerStream) {
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));
    _source->push_back(makeEvent("4"));

    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
    getExpCtx()->releasePendingChangeStreamResumeGap();

    // The stage is now on its surpassed fast path and never builds a second gap.
    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityGapBuildTest, GapBuildShardIdOnShard) {
    setShardId(ShardId("shardA"));
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));

    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
    const auto& gap = getExpCtx()->getPendingChangeStreamResumeGap();
    ASSERT_TRUE(gap.has_value());
    ASSERT_EQ((*gap)["error"]["shardId"].getStringData(), "shardA"sv);
}

TEST_F(CheckResumabilityGapBuildTest, GapBuildNotWhenTokenFound) {
    auto stage = makeStage();
    _source->push_back(makeEvent("2"));
    _source->push_back(makeEvent("3"));

    ASSERT_TRUE(stage->getNext().isAdvanced());
    ASSERT_TRUE(stage->getNext().isAdvanced());
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityGapBuildTest, GapBuildNotWithoutFloorOrOptIn) {
    {
        // Opted in, but a fresh context has no floor.
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        DocumentSourceChangeStreamSpec spec;
        spec.setShowErrorsAsEvents(true);
        spec.setStartAfter(ResumeToken(makeToken("2")));
        expCtx->setChangeStreamSpec(spec);
        expCtx->setMongoProcessInterface(std::make_shared<ShardIdProcessInterface>(boost::none));
        auto ds = DocumentSourceChangeStreamCheckResumability::create(expCtx, spec);
        auto stage = boost::dynamic_pointer_cast<exec::agg::ChangeStreamCheckResumabilityStage>(
            exec::agg::buildStage(ds));
        auto source = make_intrusive<ThrowingSourceStage>(expCtx);
        source->push_back(makeEvent("3"));
        exec::agg::MockStage::setSource_forTest(stage, source.get());
        ASSERT_TRUE(stage->getNext().isAdvanced());
        ASSERT_FALSE(expCtx->getPendingChangeStreamResumeGap().has_value());
    }
    {
        setShowErrorsAsEvents(false);
        auto stage = makeStage();
        _source->push_back(makeEvent("3"));
        ASSERT_TRUE(stage->getNext().isAdvanced());
        ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
    }
}

TEST_F(CheckResumabilityGapBuildTest, GapBuildConvertErrorsStageDeliversGapWithoutError) {
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));

    auto converter = make_intrusive<exec::agg::ChangeStreamConvertErrorsStage>(
        "$_internalChangeStreamConvertErrors"sv, getExpCtx());
    exec::agg::MockStage::setSource_forTest(converter, stage.get());

    auto result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    const auto doc = result.getDocument();
    ASSERT_EQ(doc["operationType"].getStringData(), "error"sv);
    ASSERT_EQ(doc["error"]["source"].getStringData(), "resume-gap"sv);
    ASSERT_DOCUMENT_EQ(doc["_id"].getDocument(), ResumeToken(makeToken("2")).toDocument());
}

TEST_F(CheckResumabilityGapBuildTest, GapEofBuildsGapAtEndOfStream) {
    auto stage = makeStage();
    _source->push_back(makeEvent("1"));

    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
    const auto& gap = getExpCtx()->getPendingChangeStreamResumeGap();
    ASSERT_TRUE(gap.has_value());
    ASSERT_DOCUMENT_EQ((*gap)["_id"].getDocument(), ResumeToken(makeToken("2")).toDocument());
    ASSERT_EQ((*gap)["error"]["earliestOplogTimestamp"].getTimestamp(), kFloor);
    getExpCtx()->releasePendingChangeStreamResumeGap();

    // Nothing was displaced, and the gap is reported only once.
    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityGapBuildTest, GapEofPauseDoesNotBuildGap) {
    auto stage = makeStage();
    _source->push_back(exec::agg::GetNextResult::makePauseExecution());

    ASSERT_TRUE(stage->getNext().isPaused());
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());

    // The gap is built once the stream really ends.
    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
}

TEST_F(CheckResumabilityGapBuildTest, GapEofNotWhenTokenFound) {
    auto stage = makeStage();
    _source->push_back(makeEvent("2"));

    ASSERT_TRUE(stage->getNext().isAdvanced());
    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityGapBuildTest, GapEofNotWithoutFloor) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceChangeStreamSpec spec;
    spec.setShowErrorsAsEvents(true);
    spec.setStartAfter(ResumeToken(makeToken("2")));
    expCtx->setChangeStreamSpec(spec);
    expCtx->setMongoProcessInterface(std::make_shared<ShardIdProcessInterface>(boost::none));
    auto ds = DocumentSourceChangeStreamCheckResumability::create(expCtx, spec);
    auto stage = boost::dynamic_pointer_cast<exec::agg::ChangeStreamCheckResumabilityStage>(
        exec::agg::buildStage(ds));
    auto source = make_intrusive<ThrowingSourceStage>(expCtx);
    exec::agg::MockStage::setSource_forTest(stage, source.get());

    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_FALSE(expCtx->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityGapBuildTest, GapEofNotWhenNotOptedIn) {
    setShowErrorsAsEvents(false);
    auto stage = makeStage();

    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

/**
 * A stage which fails with a convertible error for every event it pulls from its source,
 * modelling a failing user stage above the detector.
 */
class FailOnEventStage final : public exec::agg::Stage {
public:
    explicit FailOnEventStage(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : exec::agg::Stage("$failOnEvent"sv, expCtx) {}

protected:
    exec::agg::GetNextResult doGetNext() final {
        auto result = pSource->getNext();
        if (result.isAdvanced()) {
            uasserted(ErrorCodes::TypeMismatch, "stage failure");
        }
        return result;
    }
};

TEST_F(CheckResumabilityGapBuildTest, HeldInputIsNextResultAfterGap) {
    auto stage = makeStage();
    _source->push_back(makeEvent("1"));
    _source->push_back(makeEvent("3"));
    _source->push_back(makeEvent("4"));

    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
    getExpCtx()->releasePendingChangeStreamResumeGap();

    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument()["_id"].getDocument(),
                       ResumeToken(makeToken("3")).toDocument());
    // The identity is recorded again for stages above.
    ASSERT_TRUE(getExpCtx()->getActiveChangeStreamEvent().has_value());
    ASSERT_DOCUMENT_EQ((*getExpCtx()->getActiveChangeStreamEvent())["_id"].getDocument(),
                       ResumeToken(makeToken("3")).toDocument());

    // Then the stage continues with the rest of its input.
    result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument()["_id"].getDocument(),
                       ResumeToken(makeToken("4")).toDocument());
    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(CheckResumabilityGapBuildTest, HeldInputPullsNothingFromSourceBeforeDelivery) {
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));
    // If the source were consulted before the held input, this error would surface instead.
    _source->pushError({ErrorCodes::InternalError, "must not be pulled"});

    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
    getExpCtx()->releasePendingChangeStreamResumeGap();

    ASSERT_TRUE(stage->getNext().isAdvanced());
}

TEST_F(CheckResumabilityGapBuildTest, HeldInputFailureAboveIsAttributedToHeldEvent) {
    auto stage = makeStage();
    _source->push_back(makeEvent("3"));

    auto failing = make_intrusive<FailOnEventStage>(getExpCtx());
    exec::agg::MockStage::setSource_forTest(failing, stage.get());
    auto converter = make_intrusive<exec::agg::ChangeStreamConvertErrorsStage>(
        "$_internalChangeStreamConvertErrors"sv, getExpCtx());
    exec::agg::MockStage::setSource_forTest(converter, failing.get());

    // The gap is delivered first.
    auto result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_EQ(result.getDocument()["error"]["source"].getStringData(), "resume-gap"sv);

    // The held event is not skipped; the stage above fails on it and the error event carries its
    // token.
    result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    const auto doc = result.getDocument();
    ASSERT_EQ(doc["operationType"].getStringData(), "error"sv);
    ASSERT_EQ(doc["error"]["code"].getInt(), static_cast<int>(ErrorCodes::TypeMismatch));
    ASSERT_DOCUMENT_EQ(doc["_id"].getDocument(), ResumeToken(makeToken("3")).toDocument());
}

class CheckResumabilityErrorEventPositionTest : public CheckResumabilityGapBuildTest {
protected:
    static bool isErrorEventResult(const exec::agg::GetNextResult& result) {
        return result.isAdvanced() &&
            result.getDocument()["operationType"].getType() == BSONType::string &&
            result.getDocument()["operationType"].getStringData() == "error"sv;
    }

    Document makeGapForToken() const {
        const Value token(ResumeToken(makeToken("2")).toDocument());
        MutableDocument gap;
        gap.setField("operationType"sv, Value("error"sv));
        gap.setField("_id"sv, token);
        gap.setField("error"sv, Value(Document{{"source"sv, "resume-gap"sv}}));
        gap.metadata().setSortKey(token, true);
        return gap.freeze();
    }
};

TEST_F(CheckResumabilityErrorEventPositionTest, ErrorEventPositionGapForTokenIsForwarded) {
    auto stage = makeStage();
    // One gap per shard; none is deduplicated.
    _source->push_back(makeGapForToken());
    _source->push_back(makeGapForToken());
    _source->push_back(makeEvent("3"));

    for (int i = 0; i < 2; ++i) {
        auto result = stage->getNext();
        ASSERT_TRUE(result.isAdvanced());
        ASSERT_EQ(result.getDocument()["error"]["source"].getStringData(), "resume-gap"sv);
    }
    // The gap's sort key is the client's token, so forwarding it must also advance the resume
    // status to kFoundToken (the router's ensure-resume-token-present stage relies on this).
    ASSERT_EQ(stage->getResumeStatusForTest(),
              exec::agg::ChangeStreamCheckResumabilityStage::ResumeStatus::kFoundToken);
    // The resume point is accounted for, so no gap is built for the next event.
    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_FALSE(isErrorEventResult(result));
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityErrorEventPositionTest, ErrorEventPositionWithoutSortKeyIsForwarded) {
    auto stage = makeStage();
    _source->push_back(makeErrorEvent("1", false /* withSortKey */));
    _source->push_back(makeEvent("3"));

    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_FALSE(result.getDocument().metadata().hasSortKey());
    ASSERT_EQ(result.getDocument()["operationType"].getStringData(), "error"sv);

    // The resume state is unchanged: the next event still surpasses the token without it having
    // been found or accounted for, so the gap is built.
    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
}

TEST_F(CheckResumabilityErrorEventPositionTest, ErrorEventPositionBeforeTokenIsSwallowed) {
    auto stage = makeStage();
    _source->push_back(makeErrorEvent("1"));
    _source->push_back(makeEvent("2"));

    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_FALSE(isErrorEventResult(result));
    ASSERT_DOCUMENT_EQ(result.getDocument()["_id"].getDocument(),
                       ResumeToken(makeToken("2")).toDocument());
}

TEST_F(CheckResumabilityErrorEventPositionTest, ErrorEventPositionAtTokenIsForwardedAndFound) {
    auto stage = makeStage();
    _source->push_back(makeErrorEvent("2"));
    _source->push_back(makeEvent("3"));

    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_TRUE(isErrorEventResult(result));

    // The token was found, so the following event does not build a gap.
    result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_FALSE(isErrorEventResult(result));
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityErrorEventPositionTest, ErrorEventPositionAfterTokenIsHeldBehindGap) {
    auto stage = makeStage();
    _source->push_back(makeErrorEvent("3"));

    // The conditions for a gap hold, so the gap is signalled ahead of the error event.
    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamHistoryLost);
    ASSERT_TRUE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
    getExpCtx()->releasePendingChangeStreamResumeGap();

    // The error event is delivered next.
    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_TRUE(isErrorEventResult(result));
    ASSERT_DOCUMENT_EQ(result.getDocument()["_id"].getDocument(),
                       ResumeToken(makeToken("3")).toDocument());
    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(CheckResumabilityErrorEventPositionTest, ErrorEventPositionAfterTokenIsForwardedOtherwise) {
    // No floor recorded: the error event is forwarded and no gap is built.
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceChangeStreamSpec spec;
    spec.setShowErrorsAsEvents(true);
    spec.setStartAfter(ResumeToken(makeToken("2")));
    expCtx->setChangeStreamSpec(spec);
    expCtx->setMongoProcessInterface(std::make_shared<ShardIdProcessInterface>(boost::none));
    auto ds = DocumentSourceChangeStreamCheckResumability::create(expCtx, spec);
    auto stage = boost::dynamic_pointer_cast<exec::agg::ChangeStreamCheckResumabilityStage>(
        exec::agg::buildStage(ds));
    auto source = make_intrusive<ThrowingSourceStage>(expCtx);
    source->push_back(makeErrorEvent("3"));
    exec::agg::MockStage::setSource_forTest(stage, source.get());

    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_TRUE(isErrorEventResult(result));
    ASSERT_FALSE(expCtx->getPendingChangeStreamResumeGap().has_value());

    // Also forwarded when the resume point is already accounted for.
    auto stage2 = makeStage();
    _source->push_back(makeEvent("2"));
    _source->push_back(makeErrorEvent("3"));
    ASSERT_TRUE(stage2->getNext().isAdvanced());
    result = stage2->getNext();
    ASSERT_TRUE(isErrorEventResult(result));
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());
}

TEST_F(CheckResumabilityErrorEventPositionTest, ErrorEventPositionUnsplitFragmentFailsClosed) {
    // The client's token names fragment 1 of the event; the error event's token has no fragment.
    auto clientToken = makeToken("2", 2 /* version */);
    clientToken.fragmentNum = 1;
    DocumentSourceChangeStreamSpec spec;
    spec.setStartAfter(ResumeToken(clientToken));
    auto ds = DocumentSourceChangeStreamCheckResumability::create(getExpCtx(), spec);
    auto stage = boost::dynamic_pointer_cast<exec::agg::ChangeStreamCheckResumabilityStage>(
        exec::agg::buildStage(ds));
    auto source = make_intrusive<ThrowingSourceStage>(getExpCtx());
    source->push_back(makeErrorEvent("2", true /* withSortKey */, 2 /* version */));
    exec::agg::MockStage::setSource_forTest(stage, source.get());

    ASSERT_EQ(getSignalCode(stage), ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumabilityErrorEventPositionTest,
       ErrorEventPositionNotOptedInUsesOrdinaryComparison) {
    setShowErrorsAsEvents(false);

    // Before the token: swallowed. After the token: forwarded, with no gap built even though a
    // floor is recorded.
    auto stage = makeStage();
    _source->push_back(makeErrorEvent("1"));
    _source->push_back(makeErrorEvent("3"));
    auto result = stage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument()["_id"].getDocument(),
                       ResumeToken(makeToken("3")).toDocument());
    ASSERT_FALSE(getExpCtx()->getPendingChangeStreamResumeGap().has_value());

    // A fragment mismatch does not fail the stream; the event is passed on for splitting.
    auto clientToken = makeToken("2", 2 /* version */);
    clientToken.fragmentNum = 1;
    DocumentSourceChangeStreamSpec spec;
    spec.setStartAfter(ResumeToken(clientToken));
    auto ds = DocumentSourceChangeStreamCheckResumability::create(getExpCtx(), spec);
    auto splitStage = boost::dynamic_pointer_cast<exec::agg::ChangeStreamCheckResumabilityStage>(
        exec::agg::buildStage(ds));
    auto source = make_intrusive<ThrowingSourceStage>(getExpCtx());
    source->push_back(makeErrorEvent("2", true /* withSortKey */, 2 /* version */));
    exec::agg::MockStage::setSource_forTest(splitStage, source.get());
    ASSERT_TRUE(splitStage->getNext().isAdvanced());
}

}  // namespace
}  // namespace mongo
