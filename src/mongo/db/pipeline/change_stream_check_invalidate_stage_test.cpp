// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_check_invalidate_stage.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_stage_test_fixture.h"
#include "mongo/db/pipeline/change_stream_test_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <deque>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using exec::agg::ChangeStreamCheckInvalidateStage;
using exec::agg::GetNextResult;
using exec::agg::MockStage;
using namespace change_stream_test_helper;

// Builds an invalidating command event (a "drop" by default) carrying 'ts' as its cluster time and
// a matching resume token as its sort key.
Document makeInvalidationEvent(Timestamp ts, std::string_view operationType = "drop") {
    auto token = makeResumeToken(
        ts, testUuid(), Value(), operationType, ResumeTokenData::FromInvalidate::kFromInvalidate);
    MutableDocument doc(Document{
        {DocumentSourceChangeStream::kOperationTypeField, Value(std::string(operationType))},
        {DocumentSourceChangeStream::kClusterTimeField, Value(ts)},
        {DocumentSourceChangeStream::kWallTimeField, Value(Date_t())}});
    doc.metadata().setSortKey(Value(token), /*isSingleElementKey=*/true);
    return doc.freeze();
}

boost::intrusive_ptr<ChangeStreamCheckInvalidateStage> makeStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<ResumeTokenData> startAfterInvalidate,
    boost::optional<ResumeTokenData> resumePoint) {
    return make_intrusive<ChangeStreamCheckInvalidateStage>(
        DocumentSourceChangeStreamCheckInvalidate::kStageName,
        expCtx,
        std::move(startAfterInvalidate),
        std::move(resumePoint));
}

// Builds an event resume token (an "insert" by default) for use as a stream's resume point.
ResumeTokenData makeResumePoint(Timestamp ts, std::string_view operationType = "insert") {
    return ResumeToken::parse(makeResumeToken(ts, testUuid(), Value(), operationType)).getData();
}

boost::intrusive_ptr<MockStage> setSource(
    std::deque<GetNextResult> inputs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<ChangeStreamCheckInvalidateStage>& stage) {
    auto mock = MockStage::createForTest(std::move(inputs), expCtx);
    MockStage::setSource_forTest(stage, mock.get());
    return mock;
}

std::string operationTypeOf(const GetNextResult& result) {
    return std::string{
        result.getDocument()[DocumentSourceChangeStream::kOperationTypeField].getStringData()};
}

class ChangeStreamCheckInvalidateStageTest : public ChangeStreamStageTest {};

// An invalidating command before the stream's resume point was already passed by the stream, so it
// must not raise an invalidation.
TEST_F(ChangeStreamCheckInvalidateStageTest, SwallowsInvalidatingCommandBeforeResumePoint) {
    const Timestamp resumePoint(100, 1);
    auto stage = makeStage(getExpCtx(), boost::none, makeResumePoint(resumePoint));
    auto mock =
        setSource({GetNextResult(makeInvalidationEvent(Timestamp(99, 1)))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kDropCollectionOpType);

    // The invalidation was swallowed: no invalidate event follows, and the source is exhausted.
    ASSERT_TRUE(stage->getNext().isEOF());
}

// An invalidating command at the resume point is a genuine new invalidation.
TEST_F(ChangeStreamCheckInvalidateStageTest, GeneratesInvalidateAtResumePoint) {
    const Timestamp resumePoint(100, 1);
    auto stage = makeStage(
        getExpCtx(),
        boost::none,
        ResumeToken::makeHighWaterMarkToken(resumePoint, ResumeTokenData::kDefaultTokenVersion)
            .getData());
    auto mock = setSource({GetNextResult(makeInvalidationEvent(resumePoint))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());

    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

// An invalidating command at the same cluster time as a non-invalidate resume point sorts after it:
// the invalidate notification the command produces carries the 'fromInvalidate' bit, which orders
// after the resume point's non-invalidate token. The command is therefore a genuinely new
// invalidation, even though its raw event identity ("drop" sorts before "insert") does not say so.
TEST_F(ChangeStreamCheckInvalidateStageTest,
       GeneratesSameTimestampInvalidateAfterNonInvalidateResumePoint) {
    const Timestamp resumePoint(100, 1);
    auto stage = makeStage(getExpCtx(), boost::none, makeResumePoint(resumePoint, "insert"));
    auto mock = setSource({GetNextResult(makeInvalidationEvent(resumePoint))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kDropCollectionOpType);

    // The command sorts after the resume point, so it is reported as a new invalidation.
    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

// An invalidating command that sorts before an invalidate resume point at the same cluster time was
// already passed by the stream, so it must not raise a second invalidation. Both tokens carry the
// invalidate flag, so their event identities decide the order ("drop" sorts before "rename").
TEST_F(ChangeStreamCheckInvalidateStageTest,
       SwallowsSameTimestampCommandBeforeInvalidateResumePoint) {
    const Timestamp resumePoint(100, 1);
    auto resumePointToken = ResumeToken::parse(makeResumeToken(
        resumePoint, testUuid(), Value(), "rename", ResumeTokenData::kFromInvalidate));
    auto stage = makeStage(getExpCtx(), boost::none, resumePointToken.getData());
    auto mock =
        setSource({GetNextResult(makeInvalidationEvent(resumePoint, "drop"))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kDropCollectionOpType);

    // The command sorts before the resume point, so it was already passed and is swallowed.
    ASSERT_TRUE(stage->getNext().isEOF());
}

// A non-invalidate resume point that is the invalidating command's own token: the client resumed
// from the drop (or rename) event rather than from an invalidate notification, so the command is
// the invalidating event the stream must still report, matching a stream that was not rebuilt.
TEST_F(ChangeStreamCheckInvalidateStageTest, GeneratesInvalidateAtNonInvalidateResumePoint) {
    const Timestamp resumePoint(100, 1);
    // The drop's own token, without the invalidate bit.
    auto dropToken =
        ResumeToken::parse(makeResumeToken(resumePoint, testUuid(), Value(), "drop")).getData();
    auto stage = makeStage(getExpCtx(), boost::none, dropToken);
    auto mock = setSource({GetNextResult(makeInvalidationEvent(resumePoint))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kDropCollectionOpType);

    // The command is reported as a new invalidation.
    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

// An invalidating command after the resume point is a genuine new invalidation.
TEST_F(ChangeStreamCheckInvalidateStageTest, GeneratesInvalidateAfterResumePoint) {
    const Timestamp resumePoint(100, 1);
    auto stage = makeStage(getExpCtx(), boost::none, makeResumePoint(resumePoint));
    auto mock =
        setSource({GetNextResult(makeInvalidationEvent(Timestamp(101, 1)))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());

    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

Document makeInvalidationEventVersioned(Timestamp ts,
                                        int version,
                                        std::string_view operationType = "drop") {
    auto eventId =
        Value(Document{{"operationType", operationType}, {"operationDescription", Value()}});
    ResumeTokenData data{ts,
                         version,
                         0,
                         testUuid(),
                         eventId,
                         ResumeTokenData::kFromInvalidate,
                         ResumeTokenData::kEventToken};
    auto token = ResumeToken(data).toDocument();
    MutableDocument doc(Document{
        {DocumentSourceChangeStream::kOperationTypeField, Value(std::string(operationType))},
        {DocumentSourceChangeStream::kClusterTimeField, Value(ts)},
        {DocumentSourceChangeStream::kWallTimeField, Value(Date_t())}});
    doc.metadata().setSortKey(Value(token), /*isSingleElementKey=*/true);
    return doc.freeze();
}

ResumeTokenData makeInvalidateResumePointVersioned(Timestamp ts,
                                                   int version,
                                                   std::string_view operationType = "drop") {
    auto eventId =
        Value(Document{{"operationType", operationType}, {"operationDescription", Value()}});
    return ResumeTokenData{ts,
                           version,
                           0,
                           testUuid(),
                           eventId,
                           ResumeTokenData::kFromInvalidate,
                           ResumeTokenData::kEventToken};
}

// The event identity layout is version-specific, so a command whose version differs from the resume
// point cannot be ordered against it by identity. Such a command is treated as history the stream
// has already passed rather than risking a spurious invalidation.
TEST_F(ChangeStreamCheckInvalidateStageTest, SwallowsVersionMismatchedCommandAtResumePoint) {
    const Timestamp resumePoint(100, 1);
    // The client resumed with a version 2 invalidate token for a "drop".
    auto stage = makeStage(
        getExpCtx(), boost::none, makeInvalidateResumePointVersioned(resumePoint, 2, "drop"));
    // The rebuilt stream observes the same command as a version 1 token. Only the version differs.
    auto mock = setSource({GetNextResult(makeInvalidationEventVersioned(resumePoint, 1, "drop"))},
                          getExpCtx(),
                          stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kDropCollectionOpType);

    // The versions differ, so the identities are not comparable: the command is swallowed and no
    // invalidate event follows.
    ASSERT_TRUE(stage->getNext().isEOF());
}

// A stream rebuilt for a backfill scans from before its resume point and keeps its 'startAfter'
// invalidate state until it reaches the resume point. The resume point and the 'startAfter' token
// are the same client token, so a different invalidating command at that cluster time sorts after
// the resume point yet is still part of the event the client already moved past. It must be
// swallowed, exactly as a stream that was not rebuilt would swallow its first event; otherwise the
// rebuild spuriously closes the stream.
TEST_F(ChangeStreamCheckInvalidateStageTest, SwallowsSameClusterTimeInvalidateAfterResumePoint) {
    const Timestamp resumePoint(100, 1);
    // The client resumed 'startAfter' a drop at this cluster time; the resume point is the same
    // client token.
    auto startAfterToken = makeResumeToken(
        resumePoint, testUuid(), Value(), "drop", ResumeTokenData::FromInvalidate::kFromInvalidate);
    auto startAfterData = ResumeToken::parse(startAfterToken).getData();
    auto stage = makeStage(getExpCtx(), startAfterData, startAfterData);
    // A different invalidating command ('rename' sorts after 'drop') at the same cluster time.
    auto mock = setSource(
        {GetNextResult(makeInvalidationEvent(resumePoint, "rename"))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kRenameCollectionOpType);

    // The invalidation was swallowed: no invalidate event follows, and the source is exhausted.
    ASSERT_TRUE(stage->getNext().isEOF());
}

// Reviewer scenario: a rebuilt stream first rethrows the resume-point invalidate for verification,
// then observes a DIFFERENT invalidating command at the same cluster time. That later command is a
// genuinely new invalidation and must close the stream, exactly as a stream that was not rebuilt
// would: the retained 'startAfter' state must not suppress it.
TEST_F(ChangeStreamCheckInvalidateStageTest,
       GeneratesSameClusterTimeInvalidateAfterResumePointEvent) {
    const Timestamp resumePoint(100, 1);
    auto startAfterToken = makeResumeToken(
        resumePoint, testUuid(), Value(), "drop", ResumeTokenData::FromInvalidate::kFromInvalidate);
    auto startAfterData = ResumeToken::parse(startAfterToken).getData();
    auto stage = makeStage(getExpCtx(), startAfterData, startAfterData);
    auto mock = setSource({GetNextResult(makeInvalidationEvent(resumePoint, "drop")),
                           GetNextResult(makeInvalidationEvent(resumePoint, "rename"))},
                          getExpCtx(),
                          stage);

    // The resume-point event ('drop') is rethrown for resume-token verification.
    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::ChangeStreamStartAfterInvalidate);

    // The different command ('rename') at the same cluster time is a new invalidation.
    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kRenameCollectionOpType);

    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

// Same sequence as above, but without a resume point (a stream that was not rebuilt). The later
// different command at the same cluster time closes the stream; the rebuilt stream must behave the
// same way.
TEST_F(ChangeStreamCheckInvalidateStageTest,
       NonRebuildGeneratesSameClusterTimeInvalidateAfterResumePointEvent) {
    const Timestamp resumePoint(100, 1);
    auto startAfterToken = makeResumeToken(
        resumePoint, testUuid(), Value(), "drop", ResumeTokenData::FromInvalidate::kFromInvalidate);
    auto startAfterData = ResumeToken::parse(startAfterToken).getData();
    auto stage = makeStage(getExpCtx(), startAfterData, boost::none);
    auto mock = setSource({GetNextResult(makeInvalidationEvent(resumePoint, "drop")),
                           GetNextResult(makeInvalidationEvent(resumePoint, "rename"))},
                          getExpCtx(),
                          stage);

    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::ChangeStreamStartAfterInvalidate);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), DocumentSourceChangeStream::kRenameCollectionOpType);

    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

// A genuinely new invalidating command at a later cluster time still closes a rebuilt stream even
// though it retains its 'startAfter' invalidate state.
TEST_F(ChangeStreamCheckInvalidateStageTest, GeneratesLaterInvalidateAfterStartAfterResumePoint) {
    const Timestamp resumePoint(100, 1);
    auto startAfterToken = makeResumeToken(
        resumePoint, testUuid(), Value(), "drop", ResumeTokenData::FromInvalidate::kFromInvalidate);
    auto startAfterData = ResumeToken::parse(startAfterToken).getData();
    auto stage = makeStage(getExpCtx(), startAfterData, startAfterData);
    auto mock =
        setSource({GetNextResult(makeInvalidationEvent(Timestamp(101, 1)))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());

    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

// Without a resume point (the normal case, where the oplog scan starts at the resume point) every
// invalidating command raises an invalidation, including one whose cluster time is in the past.
TEST_F(ChangeStreamCheckInvalidateStageTest, GeneratesInvalidateWithoutResumePoint) {
    auto stage = makeStage(getExpCtx(), boost::none, boost::none);
    auto mock =
        setSource({GetNextResult(makeInvalidationEvent(Timestamp(99, 1)))}, getExpCtx(), stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());

    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_EQ(operationTypeOf(second), DocumentSourceChangeStream::kInvalidateOpType);
}

// The resume point must not interfere with the 'startAfter' invalidate handling: an event exactly
// at the resume point that matches the startAfter token is still rethrown for verification.
TEST_F(ChangeStreamCheckInvalidateStageTest, ExactStartAfterInvalidateStillRethrows) {
    const Timestamp resumePoint(100, 1);
    auto startAfterToken = makeResumeToken(
        resumePoint, testUuid(), Value(), "drop", ResumeTokenData::FromInvalidate::kFromInvalidate);
    auto startAfterData = ResumeToken::parse(startAfterToken).getData();
    auto stage = makeStage(getExpCtx(), startAfterData, startAfterData);
    auto mock = setSource({GetNextResult(makeInvalidationEvent(resumePoint))}, getExpCtx(), stage);

    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::ChangeStreamStartAfterInvalidate);
}

// A stream rebuilt for an end-of-transaction lookback scans the oplog from before its resume point,
// so it can deliver a pre-resume event before the resume-point invalidate. That pre-resume event
// must not consume the 'startAfter' state; otherwise the resume-point invalidate is misread as a
// genuinely new invalidation and wrongly closes the stream.
TEST_F(ChangeStreamCheckInvalidateStageTest, PreResumeEventKeepsStartAfterInvalidate) {
    const Timestamp resumePoint(100, 1);
    auto startAfterToken = makeResumeToken(
        resumePoint, testUuid(), Value(), "drop", ResumeTokenData::FromInvalidate::kFromInvalidate);
    auto startAfterData = ResumeToken::parse(startAfterToken).getData();

    auto insertToken = makeResumeToken(Timestamp(99, 1), testUuid(), Value(), "insert");
    MutableDocument insert(
        Document{{DocumentSourceChangeStream::kOperationTypeField, Value(std::string("insert"))},
                 {DocumentSourceChangeStream::kClusterTimeField, Value(Timestamp(99, 1))},
                 {DocumentSourceChangeStream::kWallTimeField, Value(Date_t())}});
    insert.metadata().setSortKey(Value(insertToken), /*isSingleElementKey=*/true);

    auto stage = makeStage(getExpCtx(), startAfterData, startAfterData);
    auto mock = setSource(
        {GetNextResult(insert.freeze()), GetNextResult(makeInvalidationEvent(resumePoint))},
        getExpCtx(),
        stage);

    // The pre-resume insert is forwarded.
    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(operationTypeOf(first), "insert");

    // The resume-point invalidate must still be rethrown for resume-token verification rather than
    // queued as a new invalidation.
    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::ChangeStreamStartAfterInvalidate);
}

}  // namespace
}  // namespace mongo
