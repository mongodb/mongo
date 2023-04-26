/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/catalog/historical_catalogid_tracker.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(HistoricalCatalogIdTrackerTest, Create) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID uuid = UUID::gen();
    RecordId rid{1};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create entry
    tracker.create(nss, uuid, rid, Timestamp(1, 2));

    // Lookup without timestamp returns latest catalogId
    ASSERT_EQ(tracker.lookup(nss, boost::none).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).id, rid);
    // Lookup before create returns unknown if looking before oldest
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    // Lookup before create returns not exists if looking after oldest
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 1)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 1)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup at create returns catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 2)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 2)).id, rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 3)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 3)).id, rid);
}

TEST(HistoricalCatalogIdTrackerTest, Drop) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID uuid = UUID::gen();
    RecordId rid{1};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create and drop collection. We have a time window where the namespace exists
    tracker.create(nss, uuid, rid, Timestamp(1, 5));
    tracker.drop(nss, uuid, Timestamp(1, 10));

    // Lookup without timestamp returns none
    ASSERT_EQ(tracker.lookup(nss, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup before create and oldest returns unknown
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    // Lookup before create returns not exists
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup at create returns catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 5)).id, rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 6)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 6)).id, rid);
    // Lookup at drop returns none
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup after drop returns none
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 20)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 20)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, Rename) {
    NamespaceString from = NamespaceString::createNamespaceString_forTest("a.b");
    NamespaceString to = NamespaceString::createNamespaceString_forTest("a.c");
    UUID uuid = UUID::gen();
    RecordId rid{1};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create and rename collection. We have two windows where the collection exists but for
    // different namespaces
    tracker.create(from, uuid, rid, Timestamp(1, 5));
    tracker.rename(from, to, Timestamp(1, 10));

    // Lookup without timestamp on 'from' returns none. By 'uuid' returns catalogId
    ASSERT_EQ(tracker.lookup(from, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).id, rid);
    // Lookup before create and oldest returns unknown
    ASSERT_EQ(tracker.lookup(from, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    // Lookup before create returns not exists
    ASSERT_EQ(tracker.lookup(from, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup at create returns catalogId
    ASSERT_EQ(tracker.lookup(from, Timestamp(1, 5)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 5)).id, rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(tracker.lookup(from, Timestamp(1, 6)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 6)).id, rid);
    // Lookup at rename on 'from' returns none. By 'uuid' returns catalogId
    ASSERT_EQ(tracker.lookup(from, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 10)).id, rid);
    // Lookup after rename on 'from' returns none. By 'uuid' returns catalogId
    ASSERT_EQ(tracker.lookup(from, Timestamp(1, 20)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 20)).id, rid);

    // Lookup without timestamp on 'to' returns catalogId
    ASSERT_EQ(tracker.lookup(to, boost::none).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).id, rid);
    // Lookup before rename and oldest on 'to' returns unknown
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    // Lookup before rename on 'to' returns not exists. By 'uuid' returns catalogId
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 9)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 9)).id, rid);
    // Lookup at rename on 'to' returns catalogId
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 10)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 10)).id, rid);
    // Lookup after rename on 'to' returns catalogId
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 20)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 20)).id, rid);
}

TEST(HistoricalCatalogIdTrackerTest, RenameDropTarget) {
    NamespaceString from = NamespaceString::createNamespaceString_forTest("a.b");
    NamespaceString to = NamespaceString::createNamespaceString_forTest("a.c");
    UUID uuid = UUID::gen();
    UUID originalUUID = UUID::gen();
    RecordId rid{1};
    RecordId originalToRid{2};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create collections. The 'to' namespace will exist for one collection from Timestamp(1, 6)
    // until it is dropped by the rename at Timestamp(1, 10), after which the 'to' namespace will
    // correspond to the renamed collection.
    tracker.create(from, uuid, rid, Timestamp(1, 5));
    tracker.create(to, originalUUID, originalToRid, Timestamp(1, 6));
    // Drop and rename with the same timestamp, this is the same as dropTarget=true
    tracker.drop(to, originalUUID, Timestamp(1, 10));
    tracker.rename(from, to, Timestamp(1, 10));

    // Lookup without timestamp on 'to' and 'uuid' returns latest catalog id. By 'originalUUID'
    // returns not exists as the target was dropped.
    ASSERT_EQ(tracker.lookup(to, boost::none).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).id, rid);
    ASSERT_EQ(tracker.lookup(originalUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup before rename and oldest on 'to' returns unknown
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(originalUUID, Timestamp(1, 0)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    // Lookup before rename on 'to' returns the original rid
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 9)).id, originalToRid);
    ASSERT_EQ(tracker.lookup(originalUUID, Timestamp(1, 9)).id, originalToRid);
    // Lookup before rename on 'from' returns the rid
    ASSERT_EQ(tracker.lookup(from, Timestamp(1, 9)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 9)).id, rid);
    // Lookup at rename timestamp on 'to' and 'uuid' returns catalogId
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 10)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 10)).id, rid);
    // Lookup at rename timestamp on 'originalUUID' returns not exists as it was dropped during the
    // rename.
    ASSERT_EQ(tracker.lookup(originalUUID, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup after rename on 'to' and 'uuid' returns catalogId
    ASSERT_EQ(tracker.lookup(to, Timestamp(1, 20)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 20)).id, rid);
    // Lookup after rename timestamp on 'originalUUID' returns not exists as it was dropped during
    // the rename.
    ASSERT_EQ(tracker.lookup(originalUUID, Timestamp(1, 20)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, DropCreate) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID firstUUID = UUID::gen();
    UUID secondUUID = UUID::gen();
    RecordId rid1{1};
    RecordId rid2{2};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create, drop and recreate collection on the same namespace. We have different catalogId.
    tracker.create(nss, firstUUID, rid1, Timestamp(1, 5));
    tracker.drop(nss, firstUUID, Timestamp(1, 10));
    tracker.create(nss, secondUUID, rid2, Timestamp(1, 15));

    // Lookup without timestamp returns latest catalogId
    ASSERT_EQ(tracker.lookup(nss, boost::none).id, rid2);
    ASSERT_EQ(tracker.lookup(secondUUID, boost::none).id, rid2);
    ASSERT_EQ(tracker.lookup(firstUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup before first create returns not exists
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 4)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup at first create returns first catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).id, rid1);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).id, rid1);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup after first create returns first catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 6)).id, rid1);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 6)).id, rid1);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 6)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup at drop returns none
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 10)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup after drop returns none
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 13)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 13)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 13)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 13)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup at second create returns second catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).id, rid2);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).id, rid2);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup after second create returns second catalogId
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 20)).id, rid2);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 20)).id, rid2);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 20)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, CleanupEqDrop) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID firstUUID = UUID::gen();
    UUID secondUUID = UUID::gen();
    RecordId rid1{1};
    RecordId rid2{2};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create collection and verify we have nothing to cleanup
    tracker.create(nss, firstUUID, rid1, Timestamp(1, 5));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    tracker.drop(nss, firstUUID, Timestamp(1, 10));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 5)));
    ASSERT_TRUE(tracker.dirty(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    tracker.create(nss, secondUUID, rid2, Timestamp(1, 15));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 5)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 7)));
    ASSERT_TRUE(tracker.dirty(Timestamp(1, 10)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);

    // Cleanup at drop timestamp, advance the oldest timestamp
    tracker.cleanup(Timestamp(1, 10));

    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 10)));
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, CleanupGtDrop) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID firstUUID = UUID::gen();
    UUID secondUUID = UUID::gen();
    RecordId rid1{1};
    RecordId rid2{2};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create collection and verify we have nothing to cleanup
    tracker.create(nss, firstUUID, rid1, Timestamp(1, 5));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    tracker.drop(nss, firstUUID, Timestamp(1, 10));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 5)));
    ASSERT_TRUE(tracker.dirty(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    tracker.create(nss, secondUUID, rid2, Timestamp(1, 15));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 5)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 7)));
    ASSERT_TRUE(tracker.dirty(Timestamp(1, 12)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);

    // Cleanup after the drop timestamp
    tracker.cleanup(Timestamp(1, 12));

    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 12)));
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, CleanupGtRecreate) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID firstUUID = UUID::gen();
    UUID secondUUID = UUID::gen();
    RecordId rid1{1};
    RecordId rid2{2};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create collection and verify we have nothing to cleanup
    tracker.create(nss, firstUUID, rid1, Timestamp(1, 5));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    tracker.drop(nss, firstUUID, Timestamp(1, 10));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 5)));
    ASSERT_TRUE(tracker.dirty(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    tracker.create(nss, secondUUID, rid2, Timestamp(1, 15));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 1)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 5)));
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 7)));
    ASSERT_TRUE(tracker.dirty(Timestamp(1, 20)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);

    // Cleanup after the recreate timestamp
    tracker.cleanup(Timestamp(1, 20));

    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(tracker.dirty(Timestamp(1, 20)));
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
}

TEST(HistoricalCatalogIdTrackerTest, CleanupMultiple) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID firstUUID = UUID::gen();
    UUID secondUUID = UUID::gen();
    UUID thirdUUID = UUID::gen();
    UUID fourthUUID = UUID::gen();
    RecordId rid1{1};
    RecordId rid2{2};
    RecordId rid3{3};
    RecordId rid4{4};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create and drop multiple namespace on the same namespace
    tracker.create(nss, firstUUID, rid1, Timestamp(1, 5));
    tracker.drop(nss, firstUUID, Timestamp(1, 10));
    tracker.create(nss, secondUUID, rid2, Timestamp(1, 15));
    tracker.drop(nss, secondUUID, Timestamp(1, 20));
    tracker.create(nss, thirdUUID, rid3, Timestamp(1, 25));
    tracker.drop(nss, thirdUUID, Timestamp(1, 30));
    tracker.create(nss, fourthUUID, rid4, Timestamp(1, 35));
    tracker.drop(nss, fourthUUID, Timestamp(1, 40));

    // Lookup can find all four collections
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(thirdUUID, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(fourthUUID, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);

    // Cleanup oldest
    tracker.cleanup(Timestamp(1, 10));

    // Lookup can find the three remaining collections
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(thirdUUID, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(fourthUUID, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);

    // Cleanup
    tracker.cleanup(Timestamp(1, 21));

    // Lookup can find the two remaining collections
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(thirdUUID, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(fourthUUID, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);

    // Cleanup
    tracker.cleanup(Timestamp(1, 32));

    // Lookup can find the last remaining collections
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(thirdUUID, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(fourthUUID, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);

    // Cleanup
    tracker.cleanup(Timestamp(1, 50));

    // Lookup now result in unknown as the oldest timestamp has advanced where mapping has been
    // removed
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(thirdUUID, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(fourthUUID, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
}

TEST(HistoricalCatalogIdTrackerTest, CleanupMultipleSingleCall) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID firstUUID = UUID::gen();
    UUID secondUUID = UUID::gen();
    UUID thirdUUID = UUID::gen();
    UUID fourthUUID = UUID::gen();
    RecordId rid1{1};
    RecordId rid2{2};
    RecordId rid3{3};
    RecordId rid4{4};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create and drop multiple namespace on the same namespace
    tracker.create(nss, firstUUID, rid1, Timestamp(1, 5));
    tracker.drop(nss, firstUUID, Timestamp(1, 10));
    tracker.create(nss, secondUUID, rid2, Timestamp(1, 15));
    tracker.drop(nss, secondUUID, Timestamp(1, 20));
    tracker.create(nss, thirdUUID, rid3, Timestamp(1, 25));
    tracker.drop(nss, thirdUUID, Timestamp(1, 30));
    tracker.create(nss, fourthUUID, rid4, Timestamp(1, 35));
    tracker.drop(nss, fourthUUID, Timestamp(1, 40));

    // Lookup can find all four collections
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(thirdUUID, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(fourthUUID, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);

    // Cleanup all
    tracker.cleanup(Timestamp(1, 50));

    // Lookup now result in unknown as the oldest timestamp has advanced where mapping has been
    // removed
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(firstUUID, Timestamp(1, 5)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(secondUUID, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(thirdUUID, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(fourthUUID, Timestamp(1, 35)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
}

TEST(HistoricalCatalogIdTrackerTest, Rollback) {
    NamespaceString a = NamespaceString::createNamespaceString_forTest("b.a");
    NamespaceString b = NamespaceString::createNamespaceString_forTest("b.b");
    NamespaceString c = NamespaceString::createNamespaceString_forTest("b.c");
    NamespaceString d = NamespaceString::createNamespaceString_forTest("b.d");
    NamespaceString e = NamespaceString::createNamespaceString_forTest("b.e");

    UUID firstUUID = UUID::gen();
    UUID secondUUID = UUID::gen();
    UUID thirdUUID = UUID::gen();
    UUID fourthUUID = UUID::gen();
    UUID fifthUUID = UUID::gen();
    UUID sixthUUID = UUID::gen();
    RecordId rid1{1};
    RecordId rid2{2};
    RecordId rid3{3};
    RecordId rid4{4};
    RecordId rid5{5};
    RecordId rid6{6};

    // Initialize the oldest timestamp to (1, 1)
    HistoricalCatalogIdTracker tracker(Timestamp(1, 1));

    // Create and drop multiple namespace on the same namespace
    tracker.create(a, firstUUID, rid1, Timestamp(1, 1));
    tracker.drop(a, firstUUID, Timestamp(1, 2));
    tracker.create(a, secondUUID, rid2, Timestamp(1, 3));
    tracker.create(b, thirdUUID, rid3, Timestamp(1, 5));
    tracker.create(c, fourthUUID, rid4, Timestamp(1, 7));
    tracker.create(d, fifthUUID, rid5, Timestamp(1, 8));
    tracker.create(e, sixthUUID, rid6, Timestamp(1, 9));
    tracker.drop(b, thirdUUID, Timestamp(1, 10));

    // Rollback to Timestamp(1, 8)
    tracker.rollback(Timestamp(1, 8));

    ASSERT_EQ(tracker.lookup(e, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(firstUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(a, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(secondUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(b, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(thirdUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(c, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(fourthUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(d, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(fifthUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(e, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(sixthUUID, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, Insert) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID uuid = UUID::gen();
    RecordId rid{1};

    // Simulate startup where we have a range [oldest, stable] by initializing the oldest timestamp
    // to something high and then insert mappings behind it where the range is unknown.
    HistoricalCatalogIdTracker tracker(Timestamp(1, 40));

    // Record that the collection is known to exist
    tracker.recordExistingAtTime(nss, uuid, rid, Timestamp(1, 17));

    // Lookups before the inserted timestamp is still unknown
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 11)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 11)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);

    // Lookups at or after the inserted timestamp is found
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 25)).id, rid);

    // Record that the collection is known to exist at an even earlier timestamp
    tracker.recordExistingAtTime(nss, uuid, rid, Timestamp(1, 12));

    // We should now have extended the range from Timestamp(1, 17) to Timestamp(1, 12)
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 12)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 12)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 12)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 12)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 16)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 16)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 16)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 16)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 25)).id, rid);

    // Record that the collection is unknown to exist at an later timestamp
    tracker.recordNonExistingAtTime(nss, Timestamp(1, 25));
    tracker.recordNonExistingAtTime(uuid, Timestamp(1, 25));

    // Check the entries, most didn't change
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 22)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 22)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 22)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 22)).id, rid);
    // At Timestamp(1, 25) we now return kNotExists
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // But next timestamp returns unknown
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 26)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 26)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);

    // Record that the collection is unknown to exist at the timestamp
    tracker.recordNonExistingAtTime(nss, Timestamp(1, 26));
    tracker.recordNonExistingAtTime(uuid, Timestamp(1, 26));

    // We should not have re-written the existing entry at Timestamp(1, 26)
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 17)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 19)).id, rid);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 22)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 22)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 22)).id, rid);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 22)).id, rid);
    // At Timestamp(1, 25) we now return kNotExists
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 25)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // But next timestamp returns unknown
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 26)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 26)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 27)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 27)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);

    // Clean up, check so we are back to the original state
    tracker.cleanup(Timestamp(1, 41));

    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
}

TEST(HistoricalCatalogIdTrackerTest, InsertUnknown) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID uuid = UUID::gen();

    // Simulate startup where we have a range [oldest, stable] by initializing the oldest timestamp
    // to something high and then insert mappings behind it where the range is unknown.
    HistoricalCatalogIdTracker tracker(Timestamp(1, 40));

    // Reading before the oldest is unknown
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown);

    // Record that the collection is unknown to exist at the timestamp
    tracker.recordNonExistingAtTime(nss, Timestamp(1, 15));
    tracker.recordNonExistingAtTime(uuid, Timestamp(1, 15));

    // Lookup should now be not existing
    ASSERT_EQ(tracker.lookup(nss, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    // Lookup should now be not existing
    ASSERT_EQ(tracker.lookup(uuid, Timestamp(1, 15)).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, NoTimestamp) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    UUID uuid = UUID::gen();
    RecordId rid{1};

    HistoricalCatalogIdTracker tracker;

    // Create a collection on the namespace and confirm that we can lookup
    tracker.create(nss, uuid, rid, boost::none);
    ASSERT_EQ(tracker.lookup(nss, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);

    // Drop the collection and confirm it is also removed from mapping
    tracker.drop(nss, uuid, boost::none);
    ASSERT_EQ(tracker.lookup(nss, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, NoTimestampRename) {
    NamespaceString a = NamespaceString::createNamespaceString_forTest("a.a");
    NamespaceString b = NamespaceString::createNamespaceString_forTest("a.b");
    UUID uuid = UUID::gen();
    RecordId rid{1};

    HistoricalCatalogIdTracker tracker;

    // Create a collection on the namespace and confirm that we can lookup
    tracker.create(a, uuid, rid, boost::none);
    ASSERT_EQ(tracker.lookup(a, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(b, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);

    // Rename the collection and check lookup behavior
    tracker.rename(a, b, boost::none);
    ASSERT_EQ(tracker.lookup(a, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(b, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kExists);


    // Drop the collection and confirm it is also removed from mapping
    tracker.drop(b, uuid, boost::none);
    ASSERT_EQ(tracker.lookup(a, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(b, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuid, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

TEST(HistoricalCatalogIdTrackerTest, NoTimestampRenameDropTarget) {
    NamespaceString a = NamespaceString::createNamespaceString_forTest("a.a");
    NamespaceString b = NamespaceString::createNamespaceString_forTest("a.b");
    UUID uuidA = UUID::gen();
    UUID uuidB = UUID::gen();
    RecordId ridA{1};
    RecordId ridB{2};

    HistoricalCatalogIdTracker tracker;

    // Create collections on the namespaces and confirm that we can lookup
    tracker.create(a, uuidA, ridA, boost::none);
    tracker.create(b, uuidB, ridB, boost::none);
    auto [aId, aResult] = tracker.lookup(a, boost::none);
    auto [bId, bResult] = tracker.lookup(b, boost::none);
    ASSERT_EQ(aResult, HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(bResult, HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(aResult, tracker.lookup(uuidA, boost::none).result);
    ASSERT_EQ(bResult, tracker.lookup(uuidB, boost::none).result);
    ASSERT_EQ(aId, tracker.lookup(uuidA, boost::none).id);
    ASSERT_EQ(bId, tracker.lookup(uuidB, boost::none).id);

    // Rename the collection and check lookup behavior
    tracker.drop(b, uuidB, boost::none);
    tracker.rename(a, b, boost::none);
    auto [aIdAfter, aResultAfter] = tracker.lookup(a, boost::none);
    auto [bIdAfter, bResultAfter] = tracker.lookup(b, boost::none);
    ASSERT_EQ(aResultAfter, HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(bResultAfter, HistoricalCatalogIdTracker::LookupResult::Existence::kExists);
    ASSERT_EQ(bResultAfter, tracker.lookup(uuidA, boost::none).result);
    ASSERT_EQ(bIdAfter, tracker.lookup(uuidA, boost::none).id);
    // Verify that the the recordId on b is now what was on a. We performed a rename with
    // dropTarget=true.
    ASSERT_EQ(aId, bIdAfter);

    // Drop the collection and confirm it is also removed from mapping
    tracker.drop(b, uuidA, boost::none);
    ASSERT_EQ(tracker.lookup(a, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuidA, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(b, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
    ASSERT_EQ(tracker.lookup(uuidB, boost::none).result,
              HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists);
}

}  // namespace
}  // namespace mongo
