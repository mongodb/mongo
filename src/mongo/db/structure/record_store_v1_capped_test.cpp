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

#include "mongo/db/structure/record_store_v1_capped.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/structure/record_store_v1_test_help.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    // Provides data to be inserted. Must be large enough for largest possible record.
    // Should be in BSS so unused portions should be free.
    char zeros[20*1024*1024] = {};

    class DummyCappedDocumentDeleteCallback : public CappedDocumentDeleteCallback {
    public:
        Status aboutToDeleteCapped( OperationContext* txn, const DiskLoc& loc ) {
            deleted.push_back( loc );
            return Status::OK();
        }
        vector<DiskLoc> deleted;
    };

    void simpleInsertTest( const char* buf, int size ) {

        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;

        string myns = "test.simple1";
        CappedRecordStoreV1 rs( &txn, &cb, myns, md, &em, false );

        rs.increaseStorageSize( &txn, 1024, -1 );

        ASSERT_NOT_OK( rs.insertRecord( &txn, buf, 3, 1000 ).getStatus() );

        rs.insertRecord( &txn, buf, size, 10000 );

        {
            BSONObjBuilder b;
            int64_t storageSize = rs.storageSize( &b );
            BSONObj obj = b.obj();
            ASSERT_EQUALS( 1, obj["numExtents"].numberInt() );
            ASSERT_EQUALS( storageSize, em.quantizeExtentSize( 1024 ) );
        }

        for ( int i = 0; i < 1000; i++ ) {
            ASSERT_OK( rs.insertRecord( &txn, buf, size, 10000 ).getStatus() );
        }

        long long start = md->numRecords();
        for ( int i = 0; i < 1000; i++ ) {
            ASSERT_OK( rs.insertRecord( &txn, buf, size, 10000 ).getStatus() );
        }
        ASSERT_EQUALS( start, md->numRecords() );
        ASSERT_GREATER_THAN( start, 100 );
        ASSERT_LESS_THAN( start, 1000 );
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
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            LocAndSize records[] = {
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 1000},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1100), 900},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
            ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
            ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc().setInvalid()); // unlooped
        }
    }

    TEST(CappedRecordStoreV1, FirstLoopWithSingleExtentExactSize) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            LocAndSize records[] = {
                {DiskLoc(0, 1000), 100},
                {DiskLoc(0, 1100), 100},
                {DiskLoc(0, 1200), 100},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1500), 50},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid()); // unlooped
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1200), 100}, // first old record
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100}, // last old record
                {DiskLoc(0, 1000), 100}, // first new record
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1100), 100}, // gap after newest record XXX this is probably a bug
                {DiskLoc(0, 1500), 50}, // gap at end of extent
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
            ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
            ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
        }
    }

    TEST(CappedRecordStoreV1, NonFirstLoopWithSingleExtentExactSize) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            LocAndSize records[] = {
                {DiskLoc(0, 1000), 100},
                {DiskLoc(0, 1100), 100},
                {DiskLoc(0, 1200), 100},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1500), 50},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1200), 100}, // first old record
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100}, // last old record
                {DiskLoc(0, 1000), 100}, // first new record
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1100), 100}, // gap after newest record XXX this is probably a bug
                {DiskLoc(0, 1500), 50}, // gap at end of extent
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
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
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            LocAndSize records[] = {
                {DiskLoc(0, 1000), 100},
                {DiskLoc(0, 1100), 100},
                {DiskLoc(0, 1200), 100},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1500), 123},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1200), 100}, // first old record
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100}, // last old record
                {DiskLoc(0, 1000), 100}, // first new record
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1100), 100}, // gap after newest record
                {DiskLoc(0, 1500), 123}, // gap at end of extent
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
            ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
            ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
        }
    }

    TEST(CappedRecordStoreV1, WontLoopWith24SpareBytes) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            LocAndSize records[] = {
                {DiskLoc(0, 1000), 100},
                {DiskLoc(0, 1100), 100},
                {DiskLoc(0, 1200), 100},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1500), 124},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 100},
                {DiskLoc(0, 1100), 100},
                {DiskLoc(0, 1200), 100},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100},
                {DiskLoc(0, 1500), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1600), 24}, // gap at end of extent
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
            ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
            ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
        }
    }

    TEST(CappedRecordStoreV1, MoveToSecondExtentUnLooped) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            // Two extents, each with 1000 bytes.
            LocAndSize records[] = {
                {DiskLoc(0, 1000), 500},
                {DiskLoc(0, 1500), 300},
                {DiskLoc(0, 1800), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1900),  100},
                {DiskLoc(1, 1000), 1000},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid());
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 500},
                {DiskLoc(0, 1500), 300},
                {DiskLoc(0, 1800), 100},

                {DiskLoc(1, 1000), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1900), 100},
                {DiskLoc(1, 1100), 900},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
            ASSERT_EQUALS(md->capExtent(), DiskLoc(1, 0));
            ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc().setInvalid()); // unlooped
        }
    }

    TEST(CappedRecordStoreV1, MoveToSecondExtentLooped) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            // Two extents, each with 1000 bytes.
            LocAndSize records[] = {
                {DiskLoc(0, 1800), 100}, // old
                {DiskLoc(0, 1000), 500}, // first new
                {DiskLoc(0, 1500), 400},

                {DiskLoc(1, 1000), 300},
                {DiskLoc(1, 1300), 600},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1900), 100},
                {DiskLoc(1, 1900), 100},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc(0, 1000));
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 200 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 500},
                {DiskLoc(0, 1500), 400},

                {DiskLoc(1, 1300), 600}, // old
                {DiskLoc(1, 1000), 200}, // first new
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1800), 200},
                {DiskLoc(1, 1200), 100},
                {DiskLoc(1, 1900), 100},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
            ASSERT_EQUALS(md->capExtent(), DiskLoc(1, 0));
            ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(1, 1000));
        }
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
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            // Starting with a single empty 1000 byte extent.
            LocAndSize records[] = {
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 1000},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid()); // unlooped
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 500 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 300 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 400 - Record::HeaderSize, 0); // won't fit at end so wraps
        rs.insertRecord(&txn, zeros, 120 - Record::HeaderSize, 0); // fits at end
        rs.insertRecord(&txn, zeros,  60 - Record::HeaderSize, 0); // fits in earlier hole

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1500), 300}, // 2nd insert
                {DiskLoc(0, 1000), 400}, // 3rd (1st new)
                {DiskLoc(0, 1800), 120}, // 4th
                {DiskLoc(0, 1400),  60}, // 5th
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1460), 40},
                {DiskLoc(0, 1920), 80},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
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
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;
        CappedRecordStoreV1 rs(&txn, &cb, "test.foo", md, &em, false);

        {
            // Starting with a single empty 1000 byte extent.
            LocAndSize records[] = {
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 1000},
                {}
            };
            md->setCapExtent(&txn, DiskLoc(0, 0));
            md->setCapFirstNewRecord(&txn, DiskLoc().setInvalid()); // unlooped
            initializeV1RS(&txn, records, drecs, &em, md);
        }

        // This list of sizes was empirically generated to achieve this outcome. Don't think too
        // much about them.
        rs.insertRecord(&txn, zeros, 500 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 300 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 304 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 76 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 96 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 76 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 200 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 200 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 56 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 96 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 104 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 96 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 60 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 60 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 146 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 146 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 40 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 40 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 36 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 96 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 200 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 60 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 64 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1148), 148},
                {DiskLoc(0, 1936),  40},
                {DiskLoc(0, 1712),  40},
                {DiskLoc(0, 1296),  36},
                {DiskLoc(0, 1752), 100},
                {DiskLoc(0, 1332),  96},
                {DiskLoc(0, 1428), 200},
                {DiskLoc(0, 1852),  60},
                {DiskLoc(0, 1000),  64}, // (1st new)
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1064), 84},
                {DiskLoc(0, 1976), 24},
                {DiskLoc(0, 1912), 24},
                {DiskLoc(0, 1628), 84},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
            ASSERT_EQUALS(md->capExtent(), DiskLoc(0, 0));
            ASSERT_EQUALS(md->capFirstNewRecord(), DiskLoc(0, 1000));
        }
    }
}
