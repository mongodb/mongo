/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/storage/historical_ident_tracker.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(HistoricalIdentTracker, RecordHistoricalIdents) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(10, 10));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.b"), UUID::gen(), Timestamp(20, 20));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.c"), UUID::gen(), Timestamp(21, 21));
    tracker.recordDrop(ident, /*nss=*/NamespaceString("test.d"), UUID::gen(), Timestamp(25, 25));

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));

    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(15, 15))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));

    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    ASSERT_EQ(tracker.lookup(ident, Timestamp(21, 21))->first, NamespaceString("test.d"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(24, 24))->first, NamespaceString("test.d"));

    ASSERT(!tracker.lookup(ident, Timestamp(25, 25)));
    ASSERT(!tracker.lookup(ident, Timestamp::max()));
}

TEST(HistoricalIdentTracker, SkipRecordingNullTimestamps) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(1));
    tracker.recordRename(ident, /*oldNss=*/NamespaceString("test.b"), UUID::gen(), Timestamp(2));
    tracker.recordDrop(ident, /*nss=*/NamespaceString("test.c"), UUID::gen(), Timestamp(3));

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(1)));
    ASSERT(!tracker.lookup(ident, Timestamp(2)));
    ASSERT(!tracker.lookup(ident, Timestamp(3)));
    ASSERT(!tracker.lookup(ident, Timestamp::max()));
}

TEST(HistoricalIdentTracker, RemoveEntriesOlderThanSingle) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(50, 50));

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(49, 49))->first, NamespaceString("test.a"));

    ASSERT(!tracker.lookup(ident, Timestamp(50, 50)));

    tracker.removeEntriesOlderThan(Timestamp::min());
    tracker.removeEntriesOlderThan(Timestamp(49, 49));

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(49, 49))->first, NamespaceString("test.a"));

    tracker.removeEntriesOlderThan(Timestamp(50, 50));

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(49, 49)));
    ASSERT(!tracker.lookup(ident, Timestamp(50, 50)));
}

TEST(HistoricalIdentTracker, RemoveEntriesOlderThanMultiple) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(10, 10));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.b"), UUID::gen(), Timestamp(20, 20));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.c"), UUID::gen(), Timestamp(21, 21));

    tracker.removeEntriesOlderThan(Timestamp::min());
    tracker.removeEntriesOlderThan(Timestamp(5, 5));
    tracker.removeEntriesOlderThan(Timestamp(9, 9));

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.removeEntriesOlderThan(Timestamp(15, 15));

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.removeEntriesOlderThan(Timestamp(21, 21));

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT(!tracker.lookup(ident, Timestamp(10, 10)));
    ASSERT(!tracker.lookup(ident, Timestamp(19, 19)));
    ASSERT(!tracker.lookup(ident, Timestamp(20, 20)));
    ASSERT(!tracker.lookup(ident, Timestamp::max()));
}

TEST(HistoricalIdentTracker, RollbackToSingle) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(10, 10));

    tracker.rollbackTo(Timestamp(10, 10));
    tracker.rollbackTo(Timestamp::max());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));

    tracker.rollbackTo(Timestamp(9, 9));

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT(!tracker.lookup(ident, Timestamp(10, 10)));
    ASSERT(!tracker.lookup(ident, Timestamp::max()));
}

TEST(HistoricalIdentTracker, RollbackToMultiple) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(10, 10));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.b"), UUID::gen(), Timestamp(20, 20));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.c"), UUID::gen(), Timestamp(21, 21));

    tracker.rollbackTo(Timestamp::max());
    tracker.rollbackTo(Timestamp(22, 22));
    tracker.rollbackTo(Timestamp(21, 21));

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.rollbackTo(Timestamp(15, 15));

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT(!tracker.lookup(ident, Timestamp(10, 10)));
    ASSERT(!tracker.lookup(ident, Timestamp(19, 19)));
    ASSERT(!tracker.lookup(ident, Timestamp(20, 20)));

    tracker.rollbackTo(Timestamp::min());

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT(!tracker.lookup(ident, Timestamp(10, 10)));
    ASSERT(!tracker.lookup(ident, Timestamp(19, 19)));
    ASSERT(!tracker.lookup(ident, Timestamp(20, 20)));
    ASSERT(!tracker.lookup(ident, Timestamp::max()));
}

TEST(HistoricalIdentTracker, PinAndUnpinTimestamp) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(10, 10));

    tracker.pinAtTimestamp(Timestamp(5, 5));
    tracker.removeEntriesOlderThan(Timestamp::max());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));

    tracker.pinAtTimestamp(Timestamp(9, 9));
    tracker.removeEntriesOlderThan(Timestamp::max());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));

    tracker.unpin();
    tracker.removeEntriesOlderThan(Timestamp::max());

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT(!tracker.lookup(ident, Timestamp(10, 10)));
    ASSERT(!tracker.lookup(ident, Timestamp::max()));
}

TEST(HistoricalIdentTracker, PinnedTimestampRemoveEntriesOlderThan) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(10, 10));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.b"), UUID::gen(), Timestamp(20, 20));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.c"), UUID::gen(), Timestamp(21, 21));

    tracker.pinAtTimestamp(Timestamp(5, 5));
    tracker.removeEntriesOlderThan(Timestamp::max());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.pinAtTimestamp(Timestamp(9, 9));
    tracker.removeEntriesOlderThan(Timestamp(9, 9));
    tracker.removeEntriesOlderThan(Timestamp(10, 10));
    tracker.removeEntriesOlderThan(Timestamp::max());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.pinAtTimestamp(Timestamp(15, 15));
    tracker.removeEntriesOlderThan(Timestamp::max());

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.pinAtTimestamp(Timestamp(21, 21));
    tracker.removeEntriesOlderThan(Timestamp::max());

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT(!tracker.lookup(ident, Timestamp(10, 10)));
    ASSERT(!tracker.lookup(ident, Timestamp(19, 19)));
    ASSERT(!tracker.lookup(ident, Timestamp(20, 20)));
}

TEST(HistoricalIdentTracker, PinnedTimestampRollbackTo) {
    HistoricalIdentTracker tracker;

    const std::string ident = "ident";
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.a"), UUID::gen(), Timestamp(10, 10));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.b"), UUID::gen(), Timestamp(20, 20));
    tracker.recordRename(
        ident, /*oldNss=*/NamespaceString("test.c"), UUID::gen(), Timestamp(21, 21));

    tracker.pinAtTimestamp(Timestamp(30, 30));
    tracker.rollbackTo(Timestamp::min());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.pinAtTimestamp(Timestamp(21, 21));
    tracker.rollbackTo(Timestamp::min());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(20, 20))->first, NamespaceString("test.c"));

    tracker.pinAtTimestamp(Timestamp(20, 20));
    tracker.rollbackTo(Timestamp::min());

    ASSERT_EQ(tracker.lookup(ident, Timestamp::min())->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(9, 9))->first, NamespaceString("test.a"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(10, 10))->first, NamespaceString("test.b"));
    ASSERT_EQ(tracker.lookup(ident, Timestamp(19, 19))->first, NamespaceString("test.b"));
    ASSERT(!tracker.lookup(ident, Timestamp(20, 20)));


    tracker.pinAtTimestamp(Timestamp(5, 5));
    tracker.rollbackTo(Timestamp::min());

    ASSERT(!tracker.lookup(ident, Timestamp::min()));
    ASSERT(!tracker.lookup(ident, Timestamp(9, 9)));
    ASSERT(!tracker.lookup(ident, Timestamp(10, 10)));
    ASSERT(!tracker.lookup(ident, Timestamp(19, 19)));
    ASSERT(!tracker.lookup(ident, Timestamp(20, 20)));
}

}  // namespace mongo
