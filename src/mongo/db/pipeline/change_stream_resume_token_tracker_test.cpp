// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/change_stream_check_resumability_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <cstddef>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using exec::agg::ChangeStreamResumeTokenTracker;
using EventPosition = ChangeStreamResumeTokenTracker::EventPosition;

const UUID& testUuid() {
    static const UUID* uuid_gen = new UUID(UUID::gen());
    return *uuid_gen;
}

ResumeTokenData makeToken(Timestamp clusterTime, int idValue, size_t txnOpIndex = 0) {
    return ResumeTokenData(clusterTime,
                           ResumeTokenData::kDefaultTokenVersion,
                           txnOpIndex,
                           testUuid(),
                           /* eventIdentifier */ Value(Document{{"_id", idValue}}));
}

Document makeEvent(const ResumeTokenData& token) {
    MutableDocument doc;
    doc.metadata().setSortKey(Value(ResumeToken(token).toDocument()), true);
    doc.addField("operationType", Value("update"sv));
    return doc.freeze();
}

Document makeEventWithoutSortKey() {
    MutableDocument doc;
    doc.addField("operationType", Value("update"sv));
    return doc.freeze();
}

TEST(ChangeStreamResumeTokenTrackerTest, NoTokenClassifiesEveryEventAsAfterResumePoint) {
    ChangeStreamResumeTokenTracker tracker(boost::none);

    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 1), 0))),
              EventPosition::kAfterResumePoint);
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 2), 0))),
              EventPosition::kAfterResumePoint);
}

TEST(ChangeStreamResumeTokenTrackerTest, HighWaterMarkTokenClassifiesEveryEventAsAfterResumePoint) {
    const auto hwm = ResumeToken::makeHighWaterMarkToken(Timestamp(100, 1),
                                                         ResumeTokenData::kDefaultTokenVersion)
                         .getData();
    ChangeStreamResumeTokenTracker tracker(hwm);

    // A high water mark is classified by the same comparison as any other resume point. Even an
    // event at the exact clusterTime of the high water mark is past the resume point.
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 1), 0))),
              EventPosition::kAfterResumePoint);
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 5), 0))),
              EventPosition::kAfterResumePoint);
}

TEST(ChangeStreamResumeTokenTrackerTest, FoundTokenLatchesAfterResumePoint) {
    const auto token = makeToken(Timestamp(100, 1), 0);
    ChangeStreamResumeTokenTracker tracker(token);

    ASSERT_EQ(tracker.classify(makeEvent(token)), EventPosition::kAtResumePoint);
    // Once the resume point has been seen, later events are not compared any more.
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 2), 0))),
              EventPosition::kAfterResumePoint);
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 3), 0))),
              EventPosition::kAfterResumePoint);
}

TEST(ChangeStreamResumeTokenTrackerTest, BeforeResumePointDoesNotLatch) {
    // The client token is the second operation of a transaction; the first operation precedes it.
    const auto clientToken = makeToken(Timestamp(100, 1), 0, /* txnOpIndex */ 1);
    const auto beforeToken = makeToken(Timestamp(100, 1), 0, /* txnOpIndex */ 0);
    ChangeStreamResumeTokenTracker tracker(clientToken);

    ASSERT_EQ(tracker.classify(makeEvent(beforeToken)), EventPosition::kBeforeResumePoint);
    // Still comparing: the resume point itself is recognized next.
    ASSERT_EQ(tracker.classify(makeEvent(clientToken)), EventPosition::kAtResumePoint);
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 2), 0))),
              EventPosition::kAfterResumePoint);
}

TEST(ChangeStreamResumeTokenTrackerTest, BeforeResumePointByEventIdentifierDoesNotLatch) {
    // Same clusterTime and txnOpIndex, but the event's document key sorts before the client
    // token's.
    const auto clientToken = makeToken(Timestamp(100, 1), /* idValue */ 1);
    const auto beforeToken = makeToken(Timestamp(100, 1), /* idValue */ 0);
    ChangeStreamResumeTokenTracker tracker(clientToken);

    ASSERT_EQ(tracker.classify(makeEvent(beforeToken)), EventPosition::kBeforeResumePoint);
    ASSERT_EQ(tracker.classify(makeEvent(clientToken)), EventPosition::kAtResumePoint);
}

TEST(ChangeStreamResumeTokenTrackerTest, SurpassedTokenLatchesAfterResumePoint) {
    const auto clientToken = makeToken(Timestamp(100, 1), 0);
    ChangeStreamResumeTokenTracker tracker(clientToken);

    // The resume token is never observed; a later event is already past it.
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 2), 0))),
              EventPosition::kAfterResumePoint);
    // Subsequent events are not compared.
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 3), 0))),
              EventPosition::kAfterResumePoint);
}

TEST(ChangeStreamResumeTokenTrackerTest, NeedsSplitIsAfterResumePoint) {
    auto clientToken = makeToken(Timestamp(100, 1), 0);
    clientToken.fragmentNum = 0;
    ChangeStreamResumeTokenTracker tracker(clientToken);

    // The event carries no fragmentNum, so it is a candidate for splitting and is treated as past
    // the resume point (its image is still required).
    ASSERT_EQ(tracker.classify(makeEvent(makeToken(Timestamp(100, 1), 0))),
              EventPosition::kAfterResumePoint);
}

TEST(ChangeStreamResumeTokenTrackerTest, EventWithoutSortKeyIsAfterResumePoint) {
    ChangeStreamResumeTokenTracker tracker(makeToken(Timestamp(100, 1), 0));

    ASSERT_EQ(tracker.classify(makeEventWithoutSortKey()), EventPosition::kAfterResumePoint);
}

}  // namespace
}  // namespace mongo
