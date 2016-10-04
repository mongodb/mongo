// record_store_v1_capped_test.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/mmap_v1/record_store_v1_capped.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_capped_iterator.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_test_help.h"

#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

using std::string;
using std::vector;

// Provides data to be inserted. Must be large enough for largest possible record.
// Should be in BSS so unused portions should be free.
char zeros[20 * 1024 * 1024] = {};

class DummyCappedCallback : public CappedCallback {
public:
    Status aboutToDeleteCapped(OperationContext* txn, const RecordId& loc, RecordData data) {
        deleted.push_back(DiskLoc::fromRecordId(loc));
        return Status::OK();
    }

    void notifyCappedWaitersIfNeeded() {}

    vector<DiskLoc> deleted;
};

void simpleInsertTest(const char* buf, int size) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;

    string myns = "test.simple1";
    CappedRecordStoreV1 rs(&txn, &cb, myns, md, &em, false);

    rs.increaseStorageSize(&txn, 1024, false);

    ASSERT_NOT_OK(rs.insertRecord(&txn, buf, 3, 1000).getStatus());

    rs.insertRecord(&txn, buf, size, 10000);

    {
        BSONObjBuilder b;
        int64_t storageSize = rs.storageSize(&txn, &b);
        BSONObj obj = b.obj();
        ASSERT_EQUALS(1, obj["numExtents"].numberInt());
        ASSERT_EQUALS(storageSize, em.quantizeExtentSize(1024));
    }

    for (int i = 0; i < 1000; i++) {
        ASSERT_OK(rs.insertRecord(&txn, buf, size, 10000).getStatus());
    }

    long long start = md->numRecords();
    for (int i = 0; i < 1000; i++) {
        ASSERT_OK(rs.insertRecord(&txn, buf, size, 10000).getStatus());
    }
    ASSERT_EQUALS(start, md->numRecords());
    ASSERT_GREATER_THAN(start, 100);
    ASSERT_LESS_THAN(start, 1000);
}

TEST(CappedRecordStoreV1, SimpleInsertSize4) {
    simpleInsertTest("abcd", 4);
}
TEST(CappedRecordStoreV1, SimpleInsertSize8) {
    simpleInsertTest("abcdefgh", 8);
}

TEST(CappedRecordStoreV1, EmptySingleExtent) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        LocAndSize records[] = {{}};
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 1000}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 100}, {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1100), 900}, {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc().setInvalid());  // unlooped
    }
}

TEST(CappedRecordStoreV1, FirstLoopWithSingleExtentExactSize) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        LocAndSize records[] = {{DiskLoc(0, 1000), 100},
                                {DiskLoc(0, 1100), 100},
                                {DiskLoc(0, 1200), 100},
                                {DiskLoc(0, 1300), 100},
                                {DiskLoc(0, 1400), 100},
                                {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1500), 50}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());  // unlooped
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1200), 100},  // first old record
                             {DiskLoc(0, 1300), 100},
                             {DiskLoc(0, 1400), 100},  // last old record
                             {DiskLoc(0, 1000), 100},  // first new record
                             {}};
        LocAndSize drecs[] = {
            {DiskLoc(0, 1100), 100},  // gap after newest record XXX this is probably a bug
            {DiskLoc(0, 1500), 50},   // gap at end of extent
            {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
    }
}

TEST(CappedRecordStoreV1, NonFirstLoopWithSingleExtentExactSize) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        LocAndSize records[] = {{DiskLoc(0, 1000), 100},
                                {DiskLoc(0, 1100), 100},
                                {DiskLoc(0, 1200), 100},
                                {DiskLoc(0, 1300), 100},
                                {DiskLoc(0, 1400), 100},
                                {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1500), 50}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1200), 100},  // first old record
                             {DiskLoc(0, 1300), 100},
                             {DiskLoc(0, 1400), 100},  // last old record
                             {DiskLoc(0, 1000), 100},  // first new record
                             {}};
        LocAndSize drecs[] = {
            {DiskLoc(0, 1100), 100},  // gap after newest record XXX this is probably a bug
            {DiskLoc(0, 1500), 50},   // gap at end of extent
            {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
    }
}

/**
 * Current code always tries to leave 24 bytes to create a DeletedRecord.
 */
TEST(CappedRecordStoreV1, WillLoopWithout24SpareBytes) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        LocAndSize records[] = {{DiskLoc(0, 1000), 100},
                                {DiskLoc(0, 1100), 100},
                                {DiskLoc(0, 1200), 100},
                                {DiskLoc(0, 1300), 100},
                                {DiskLoc(0, 1400), 100},
                                {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1500), 123}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1200), 100},  // first old record
                             {DiskLoc(0, 1300), 100},
                             {DiskLoc(0, 1400), 100},  // last old record
                             {DiskLoc(0, 1000), 100},  // first new record
                             {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1100), 100},  // gap after newest record
                              {DiskLoc(0, 1500), 123},  // gap at end of extent
                              {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
    }
}

TEST(CappedRecordStoreV1, WontLoopWith24SpareBytes) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        LocAndSize records[] = {{DiskLoc(0, 1000), 100},
                                {DiskLoc(0, 1100), 100},
                                {DiskLoc(0, 1200), 100},
                                {DiskLoc(0, 1300), 100},
                                {DiskLoc(0, 1400), 100},
                                {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1500), 124}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 100},
                             {DiskLoc(0, 1100), 100},
                             {DiskLoc(0, 1200), 100},
                             {DiskLoc(0, 1300), 100},
                             {DiskLoc(0, 1400), 100},
                             {DiskLoc(0, 1500), 100},
                             {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1600), 24},  // gap at end of extent
                              {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
    }
}

TEST(CappedRecordStoreV1, MoveToSecondExtentUnLooped) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        // Two extents, each with 1000 bytes.
        LocAndSize records[] = {
            {DiskLoc(0, 1000), 500}, {DiskLoc(0, 1500), 300}, {DiskLoc(0, 1800), 100}, {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1900), 100}, {DiskLoc(1, 1000), 1000}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 500},
                             {DiskLoc(0, 1500), 300},
                             {DiskLoc(0, 1800), 100},

                             {DiskLoc(1, 1000), 100},
                             {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1900), 100}, {DiskLoc(1, 1100), 900}, {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(1, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc().setInvalid());  // unlooped
    }
}

TEST(CappedRecordStoreV1, MoveToSecondExtentLooped) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        // Two extents, each with 1000 bytes.
        LocAndSize records[] = {{DiskLoc(0, 1800), 100},  // old
                                {DiskLoc(0, 1000), 500},  // first new
                                {DiskLoc(0, 1500), 400},

                                {DiskLoc(1, 1000), 300},
                                {DiskLoc(1, 1300), 600},
                                {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1900), 100}, {DiskLoc(1, 1900), 100}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 200 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 500},
                             {DiskLoc(0, 1500), 400},

                             {DiskLoc(1, 1300), 600},  // old
                             {DiskLoc(1, 1000), 200},  // first new
                             {}};
        LocAndSize drecs[] = {
            {DiskLoc(0, 1800), 200}, {DiskLoc(1, 1200), 100}, {DiskLoc(1, 1900), 100}, {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(1, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(1, 1000));
    }
}

// Larger than storageSize (fails early)
TEST(CappedRecordStoreV1, OversizedRecordHuge) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        LocAndSize records[] = {{}};
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 1000}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    StatusWith<RecordId> status = rs.insertRecord(&txn, zeros, 16000, false);
    ASSERT_EQUALS(status.getStatus(), ErrorCodes::DocTooLargeForCapped);
    ASSERT_EQUALS(status.getStatus().location(), 16328);
}

// Smaller than storageSize, but larger than usable space (fails late)
TEST(CappedRecordStoreV1, OversizedRecordMedium) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        LocAndSize records[] = {{}};
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 1000}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    StatusWith<RecordId> status =
        rs.insertRecord(&txn, zeros, 1004 - MmapV1RecordHeader::HeaderSize, false);
    ASSERT_EQUALS(status.getStatus(), ErrorCodes::DocTooLargeForCapped);
    ASSERT_EQUALS(status.getStatus().location(), 28575);
}

//
// XXX The CappedRecordStoreV1Scrambler suite of tests describe existing behavior that is less
// than ideal. Any improved implementation will need to be able to handle a collection that has
// been scrambled like this.
//

/**
 * This is a minimal example that shows the current allocator laying out records out-of-order.
 */
TEST(CappedRecordStoreV1Scrambler, Minimal) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        // Starting with a single empty 1000 byte extent.
        LocAndSize records[] = {{}};
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 1000}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());  // unlooped
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    rs.insertRecord(&txn, zeros, 500 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 300 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(
        &txn, zeros, 400 - MmapV1RecordHeader::HeaderSize, false);  // won't fit at end so wraps
    rs.insertRecord(&txn, zeros, 120 - MmapV1RecordHeader::HeaderSize, false);  // fits at end
    rs.insertRecord(
        &txn, zeros, 60 - MmapV1RecordHeader::HeaderSize, false);  // fits in earlier hole

    {
        LocAndSize recs[] = {{DiskLoc(0, 1500), 300},  // 2nd insert
                             {DiskLoc(0, 1000), 400},  // 3rd (1st new)
                             {DiskLoc(0, 1800), 120},  // 4th
                             {DiskLoc(0, 1400), 60},   // 5th
                             {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1460), 40}, {DiskLoc(0, 1920), 80}, {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
    }
}

/**
 * This tests a specially crafted set of inserts that scrambles a capped collection in a way
 * that leaves 4 deleted records in a single extent.
 */
TEST(CappedRecordStoreV1Scrambler, FourDeletedRecordsInSingleExtent) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(true, 0);
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

    {
        // Starting with a single empty 1000 byte extent.
        LocAndSize records[] = {{}};
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 1000}, {}};
        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());  // unlooped
        initializeV1RS(&txn, records, drecs, NULL, &em, md);
    }

    // This list of sizes was empirically generated to achieve this outcome. Don't think too
    // much about them.
    rs.insertRecord(&txn, zeros, 500 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 300 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 304 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 76 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 96 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 76 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 200 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 200 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 56 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 96 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 104 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 96 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 60 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 60 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 146 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 146 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 40 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 40 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 36 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 100 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 96 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 200 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 60 - MmapV1RecordHeader::HeaderSize, false);
    rs.insertRecord(&txn, zeros, 64 - MmapV1RecordHeader::HeaderSize, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1148), 148},
                             {DiskLoc(0, 1936), 40},
                             {DiskLoc(0, 1712), 40},
                             {DiskLoc(0, 1296), 36},
                             {DiskLoc(0, 1752), 100},
                             {DiskLoc(0, 1332), 96},
                             {DiskLoc(0, 1428), 200},
                             {DiskLoc(0, 1852), 60},
                             {DiskLoc(0, 1000), 64},  // (1st new)
                             {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1064), 84},
                              {DiskLoc(0, 1976), 24},
                              {DiskLoc(0, 1912), 24},
                              {DiskLoc(0, 1628), 84},
                              {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
        ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
        ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
    }
}

//
// The CappedRecordStoreV1QueryStage tests some nitty-gritty capped
//  collection details.  Ported and polished from pdfiletests.cpp.
//

class CollscanHelper {
public:
    CollscanHelper(int nExtents)
        : md(new DummyRecordStoreV1MetaData(true, 0)), rs(&txn, &cb, ns(), md, &em, false) {
        LocAndSize recs[] = {{}};
        LocAndSize drecs[8];
        ASSERT_LESS_THAN(nExtents, 8);
        for (int j = 0; j < nExtents; ++j) {
            drecs[j].loc = DiskLoc(j, 1000);
            drecs[j].size = 1000;
        }
        drecs[nExtents].loc = DiskLoc();
        drecs[nExtents].size = 0;

        md->setCapExtent(&txn, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());  // unlooped
        initializeV1RS(&txn, recs, drecs, NULL, &em, md);
    }

    // Insert bypasses standard alloc/insert routines to use the extent we want.
    // TODO: Directly declare resulting record store state instead of procedurally creating it
    DiskLoc insert(const DiskLoc& ext, int i) {
        // Copied verbatim.
        BSONObjBuilder b;
        b.append("a", i);
        BSONObj o = b.done();
        int len = o.objsize();
        Extent* e = em.getExtent(ext);
        e = txn.recoveryUnit()->writing(e);
        int ofs;
        if (e->lastRecord.isNull()) {
            ofs = ext.getOfs() + (e->_extentData - (char*)e);
        } else {
            ofs = e->lastRecord.getOfs() + em.recordForV1(e->lastRecord)->lengthWithHeaders();
        }
        DiskLoc dl(ext.a(), ofs);
        MmapV1RecordHeader* r = em.recordForV1(dl);
        r = (MmapV1RecordHeader*)txn.recoveryUnit()->writingPtr(
            r, MmapV1RecordHeader::HeaderSize + len);
        r->lengthWithHeaders() = MmapV1RecordHeader::HeaderSize + len;
        r->extentOfs() = e->myLoc.getOfs();
        r->nextOfs() = DiskLoc::NullOfs;
        r->prevOfs() = e->lastRecord.isNull() ? DiskLoc::NullOfs : e->lastRecord.getOfs();
        memcpy(r->data(), o.objdata(), len);
        if (e->firstRecord.isNull())
            e->firstRecord = dl;
        else
            txn.recoveryUnit()->writingInt(em.recordForV1(e->lastRecord)->nextOfs()) = ofs;
        e->lastRecord = dl;
        return dl;
    }

    // TODO: Directly assert the desired record store state instead of just walking it
    void walkAndCount(int expectedCount) {
        // Walk the collection going forward.
        {
            CappedRecordStoreV1Iterator cursor(&txn, &rs, /*forward=*/true);
            int resultCount = 0;
            while (auto record = cursor.next()) {
                ++resultCount;
            }

            ASSERT_EQUALS(resultCount, expectedCount);
        }

        // Walk the collection going backwards.
        {
            CappedRecordStoreV1Iterator cursor(&txn, &rs, /*forward=*/false);
            int resultCount = expectedCount;
            while (auto record = cursor.next()) {
                --resultCount;
            }

            ASSERT_EQUALS(resultCount, 0);
        }
    }

    static const char* ns() {
        return "unittests.QueryStageCollectionScanCapped";
    }

    OperationContextNoop txn;
    DummyRecordStoreV1MetaData* md;
    DummyExtentManager em;

private:
    DummyCappedCallback cb;
    CappedRecordStoreV1 rs;
};


TEST(CappedRecordStoreV1QueryStage, CollscanCappedBase) {
    CollscanHelper h(1);
    h.walkAndCount(0);
}

TEST(CappedRecordStoreV1QueryStage, CollscanEmptyLooped) {
    CollscanHelper h(1);
    h.md->setCapFirstNewRecord(&h.txn, DiskLoc());
    h.walkAndCount(0);
}

TEST(CappedRecordStoreV1QueryStage, CollscanEmptyMultiExtentLooped) {
    CollscanHelper h(3);
    h.md->setCapFirstNewRecord(&h.txn, DiskLoc());
    h.walkAndCount(0);
}

TEST(CappedRecordStoreV1QueryStage, CollscanSingle) {
    CollscanHelper h(1);

    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 0));
    h.walkAndCount(1);
}

TEST(CappedRecordStoreV1QueryStage, CollscanNewCapFirst) {
    CollscanHelper h(1);
    DiskLoc x = h.insert(h.md->capExtent(), 0);
    h.md->setCapFirstNewRecord(&h.txn, x);
    h.insert(h.md->capExtent(), 1);
    h.walkAndCount(2);
}

TEST(CappedRecordStoreV1QueryStage, CollscanNewCapMiddle) {
    CollscanHelper h(1);
    h.insert(h.md->capExtent(), 0);
    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 1));
    h.insert(h.md->capExtent(), 2);
    h.walkAndCount(3);
}

TEST(CappedRecordStoreV1QueryStage, CollscanFirstExtent) {
    CollscanHelper h(2);
    h.insert(h.md->capExtent(), 0);
    h.insert(h.md->lastExtent(&h.txn), 1);
    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 2));
    h.insert(h.md->capExtent(), 3);
    h.walkAndCount(4);
}

TEST(CappedRecordStoreV1QueryStage, CollscanLastExtent) {
    CollscanHelper h(2);
    h.md->setCapExtent(&h.txn, h.md->lastExtent(&h.txn));
    h.insert(h.md->capExtent(), 0);
    h.insert(h.md->firstExtent(&h.txn), 1);
    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 2));
    h.insert(h.md->capExtent(), 3);
    h.walkAndCount(4);
}

TEST(CappedRecordStoreV1QueryStage, CollscanMidExtent) {
    CollscanHelper h(3);
    h.md->setCapExtent(&h.txn, h.em.getExtent(h.md->firstExtent(&h.txn))->xnext);
    h.insert(h.md->capExtent(), 0);
    h.insert(h.md->lastExtent(&h.txn), 1);
    h.insert(h.md->firstExtent(&h.txn), 2);
    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 3));
    h.insert(h.md->capExtent(), 4);
    h.walkAndCount(5);
}

TEST(CappedRecordStoreV1QueryStage, CollscanAloneInExtent) {
    CollscanHelper h(3);
    h.md->setCapExtent(&h.txn, h.em.getExtent(h.md->firstExtent(&h.txn))->xnext);
    h.insert(h.md->lastExtent(&h.txn), 0);
    h.insert(h.md->firstExtent(&h.txn), 1);
    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 2));
    h.walkAndCount(3);
}

TEST(CappedRecordStoreV1QueryStage, CollscanFirstInExtent) {
    CollscanHelper h(3);
    h.md->setCapExtent(&h.txn, h.em.getExtent(h.md->firstExtent(&h.txn))->xnext);
    h.insert(h.md->lastExtent(&h.txn), 0);
    h.insert(h.md->firstExtent(&h.txn), 1);
    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 2));
    h.insert(h.md->capExtent(), 3);
    h.walkAndCount(4);
}

TEST(CappedRecordStoreV1QueryStage, CollscanLastInExtent) {
    CollscanHelper h(3);
    h.md->setCapExtent(&h.txn, h.em.getExtent(h.md->firstExtent(&h.txn))->xnext);
    h.insert(h.md->capExtent(), 0);
    h.insert(h.md->lastExtent(&h.txn), 1);
    h.insert(h.md->firstExtent(&h.txn), 2);
    h.md->setCapFirstNewRecord(&h.txn, h.insert(h.md->capExtent(), 3));
    h.walkAndCount(4);
}
}
