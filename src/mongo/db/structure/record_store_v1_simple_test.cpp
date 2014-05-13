// record_store_v1_simple_test.cpp

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

#include "mongo/db/structure/record_store_v1_simple.h"

#include "mongo/db/storage/record.h"
#include "mongo/db/structure/record_store_v1_test_help.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    char zeros[20*1024*1024] = {};

    TEST( SimpleRecordStoreV1, quantizeAllocationSpaceSimple ) {
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(33),       36);
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(1000),     1024);
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(10001),    10240);
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(100000),   106496);
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(1000001),  1048576);
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(10000000), 10223616);
    }

    TEST( SimpleRecordStoreV1, quantizeAllocationMinMaxBound ) {
        const int maxSize = 16 * 1024 * 1024;
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(1), 2);
        ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(maxSize), maxSize);
    }

    /**
     * Test  Quantize record allocation on every boundary, as well as boundary-1
     *       @see NamespaceDetails::quantizeAllocationSpace()
     */
    TEST( SimpleRecordStoreV1, quantizeAllocationBoundary ) {
        for (int iBucket = 0; iBucket <= RecordStoreV1Base::MaxBucket; ++iBucket) {
            // for each bucket in range [min, max)
            const int bucketSize = RecordStoreV1Base::bucketSizes[iBucket];
            const int prevBucketSize =
                (iBucket - 1 >= 0) ? RecordStoreV1Base::bucketSizes[iBucket - 1] : 0;
            const int intervalSize = bucketSize / 16;
            for (int iBoundary = prevBucketSize;
                 iBoundary < bucketSize;
                 iBoundary += intervalSize) {
                // for each quantization boundary within the bucket
                for (int iSize = iBoundary - 1; iSize <= iBoundary; ++iSize) {
                    // test the quantization boundary - 1, and the boundary itself
                    const int quantized =
                        RecordStoreV1Base::quantizeAllocationSpace(iSize);
                    // assert quantized size is greater than or equal to requested size
                    ASSERT(quantized >= iSize);
                    // assert quantized size is within one quantization interval of
                    // the requested size
                    ASSERT(quantized - iSize <= intervalSize);
                    // assert quantization is an idempotent operation
                    ASSERT(quantized ==
                           RecordStoreV1Base::quantizeAllocationSpace(quantized));
                }
            }
        }
    }

    /**
     * For buckets up to 4MB powerOf2 allocation should round up to next power of 2. It should be
     * return the input unmodified if it is already a power of 2.
     */
    TEST( SimpleRecordStoreV1, quantizePowerOf2Small ) {
        // only tests buckets <= 4MB. Higher buckets quatize to 1MB even with powerOf2
        for (int bucket = 0; bucket < RecordStoreV1Base::MaxBucket; bucket++) {
            const int size = RecordStoreV1Base::bucketSizes[bucket];
            const int nextSize = RecordStoreV1Base::bucketSizes[bucket + 1];

            // size - 1 is quantized to size.
            ASSERT_EQUALS( size,
                           RecordStoreV1Base::quantizePowerOf2AllocationSpace( size - 1 ) );

            // size is quantized to size.
            ASSERT_EQUALS( size,
                           RecordStoreV1Base::quantizePowerOf2AllocationSpace( size ) );

            // size + 1 is quantized to nextSize (unless > 4MB which is covered by next test)
            if (size < 4*1024*1024) {
                ASSERT_EQUALS( nextSize,
                               RecordStoreV1Base::quantizePowerOf2AllocationSpace( size + 1 ) );
            }
        }
    }

    /**
     * Within the largest bucket, quantizePowerOf2AllocationSpace quantizes to the nearest
     * megabyte boundary.
     */
    TEST( SimpleRecordStoreV1, SimpleRecordLargePowerOf2ToMegabyteBoundary ) {
        // Iterate iSize over all 1mb boundaries from the size of the next to largest bucket
        // to the size of the largest bucket + 1mb.
        for( int iSize = RecordStoreV1Base::bucketSizes[ RecordStoreV1Base::MaxBucket - 1 ];
             iSize <= RecordStoreV1Base::bucketSizes[ RecordStoreV1Base::MaxBucket ] + 0x100000;
             iSize += 0x100000 ) {

            // iSize - 1 is quantized to iSize.
            ASSERT_EQUALS( iSize,
                           RecordStoreV1Base::quantizePowerOf2AllocationSpace( iSize - 1 ) );

            // iSize is quantized to iSize.
            ASSERT_EQUALS( iSize,
                           RecordStoreV1Base::quantizePowerOf2AllocationSpace( iSize ) );

            // iSize + 1 is quantized to iSize + 1mb.
            ASSERT_EQUALS( iSize + 0x100000,
                           RecordStoreV1Base::quantizePowerOf2AllocationSpace( iSize + 1 ) );
        }
    }

    BSONObj docForRecordSize( int size ) {
        BSONObjBuilder b;
        b.append( "_id", 5 );
        b.append( "x", string( size - Record::HeaderSize - 22, 'x' ) );
        BSONObj x = b.obj();
        ASSERT_EQUALS( Record::HeaderSize + x.objsize(), size );
        return x;
    }

    /** alloc() quantizes the requested size using quantizeAllocationSpace() rules. */
    TEST(SimpleRecordStoreV1, AllocQuantized) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );

        string myns = "test.AllocQuantized";
        SimpleRecordStoreV1 rs( &txn, myns, md, &em, false );

        BSONObj obj = docForRecordSize( 300 );
        StatusWith<DiskLoc> result = rs.insertRecord( &txn, obj.objdata(), obj.objsize(), 0 );
        ASSERT( result.isOK() );

        // The length of the allocated record is quantized.
        ASSERT_EQUALS( 320, rs.recordFor( result.getValue() )->lengthWithHeaders() );
    }

    /**
     * alloc() does not quantize records in index collections using quantizeAllocationSpace()
     * rules.
     */
    TEST(SimpleRecordStoreV1, AllocIndexNamespaceNotQuantized) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );

        string myns = "test.AllocIndexNamespaceNotQuantized";
        SimpleRecordStoreV1 rs( &txn, myns + "$x", md, &em, false );

        BSONObj obj = docForRecordSize( 300 );
        StatusWith<DiskLoc> result = rs.insertRecord(&txn,  obj.objdata(), obj.objsize(), 0 );
        ASSERT( result.isOK() );

        // The length of the allocated record is not quantized.
        ASSERT_EQUALS( 300, rs.recordFor( result.getValue() )->lengthWithHeaders() );

    }

    /** alloc() quantizes records in index collections to the nearest multiple of 4. */
    TEST(SimpleRecordStoreV1, AllocIndexNamespaceSlightlyQuantized) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );

        string myns = "test.AllocIndexNamespaceNotQuantized";
        SimpleRecordStoreV1 rs( &txn, myns + "$x", md, &em, false );

        BSONObj obj = docForRecordSize( 298 );
        StatusWith<DiskLoc> result = rs.insertRecord( &txn, obj.objdata(), obj.objsize(), 0 );
        ASSERT( result.isOK() );

        ASSERT_EQUALS( 300, rs.recordFor( result.getValue() )->lengthWithHeaders() );
    }

    /** alloc() returns a non quantized record larger than the requested size. */
    TEST(SimpleRecordStoreV1, AllocUseNonQuantizedDeletedRecord) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 310},
                {}
            };
            initializeV1RS(&txn, NULL, drecs, &em, md);
        }

        BSONObj obj = docForRecordSize( 300 );
        StatusWith<DiskLoc> actualLocation = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), 0);
        ASSERT_OK( actualLocation.getStatus() );

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 310},
                {}
            };
            LocAndSize drecs[] = {
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

    /** alloc() returns a non quantized record equal to the requested size. */
    TEST(SimpleRecordStoreV1, AllocExactSizeNonQuantizedDeletedRecord) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 300},
                {}
            };
            initializeV1RS(&txn, NULL, drecs, &em, md);
        }

        BSONObj obj = docForRecordSize( 300 );
        StatusWith<DiskLoc> actualLocation = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), 0);
        ASSERT_OK( actualLocation.getStatus() );

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 300},
                {}
            };
            LocAndSize drecs[] = {
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

    /**
     * alloc() returns a non quantized record equal to the quantized size plus some extra space
     * too small to make a DeletedRecord.
     */
    TEST(SimpleRecordStoreV1, AllocQuantizedWithExtra) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 343},
                {}
            };
            initializeV1RS(&txn, NULL, drecs, &em, md);
        }

        BSONObj obj = docForRecordSize( 300 );
        StatusWith<DiskLoc> actualLocation = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), 0);
        ASSERT_OK( actualLocation.getStatus() );

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 343},
                {}
            };
            LocAndSize drecs[] = {
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

    /**
     * alloc() returns a quantized record when the extra space in the reclaimed deleted record
     * is large enough to form a new deleted record.
     */
    TEST(SimpleRecordStoreV1, AllocQuantizedWithoutExtra) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 344},
                {}
            };
            initializeV1RS(&txn, NULL, drecs, &em, md);
        }


        BSONObj obj = docForRecordSize( 300 );
        StatusWith<DiskLoc> actualLocation = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), 0);
        ASSERT_OK( actualLocation.getStatus() );

        {
            LocAndSize recs[] = {
                // The returned record is quantized from 300 to 320.
                {DiskLoc(0, 1000), 320},
                {}
            };
            LocAndSize drecs[] = {
                // A new 24 byte deleted record is split off.
                {DiskLoc(0, 1320), 24},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

    /**
     * A non quantized deleted record within 1/8 of the requested size is returned as is, even
     * if a quantized portion of the deleted record could be used instead.
     */
    TEST(SimpleRecordStoreV1, AllocNotQuantizedNearDeletedSize) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000), 344},
                {}
            };
            initializeV1RS(&txn, NULL, drecs, &em, md);
        }

        BSONObj obj = docForRecordSize( 319 );
        StatusWith<DiskLoc> actualLocation = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), 0);
        ASSERT_OK( actualLocation.getStatus() );

        // Even though 319 would be quantized to 320 and 344 - 320 == 24 could become a new
        // deleted record, the entire deleted record is returned because
        // ( 344 - 320 ) < ( 320 / 8 ).

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 344},
                {}
            };
            LocAndSize drecs[] = {
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

    /** getRecordAllocationSize() returns its argument when the padding factor is 1.0. */
    TEST(SimpleRecordStoreV1, GetRecordAllocationSizeNoPadding) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );
        ASSERT_EQUALS( 1.0, md->paddingFactor() );
        ASSERT_EQUALS( 300, rs.getRecordAllocationSize( 300 ) );
    }

    /** getRecordAllocationSize() multiplies by a padding factor > 1.0. */
    TEST(SimpleRecordStoreV1, GetRecordAllocationSizeWithPadding) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );
        double paddingFactor = 1.2;
        md->setPaddingFactor( &txn, paddingFactor );
        ASSERT_EQUALS( paddingFactor, md->paddingFactor() );
        ASSERT_EQUALS( int(300 * paddingFactor), rs.getRecordAllocationSize( 300 ) );
    }

    /**
     * getRecordAllocationSize() quantizes to the nearest power of 2 when Flag_UsePowerOf2Sizes
     * is set.
     */
    TEST(SimpleRecordStoreV1, GetRecordAllocationSizePowerOf2) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(
                                                false,
                                                RecordStoreV1Base::Flag_UsePowerOf2Sizes );

        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );
        ASSERT_EQUALS( 512, rs.getRecordAllocationSize( 300 ) );
    }

    /**
     * getRecordAllocationSize() quantizes to the nearest power of 2 when Flag_UsePowerOf2Sizes
     * is set, ignoring the padding factor.
     */
    TEST(SimpleRecordStoreV1, GetRecordAllocationSizePowerOf2PaddingIgnored) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(
                                                false,
                                                RecordStoreV1Base::Flag_UsePowerOf2Sizes );

        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );
        md->setPaddingFactor( &txn, 2.0 );
        ASSERT_EQUALS( 2.0, md->paddingFactor() );
        ASSERT_EQUALS( 512, rs.getRecordAllocationSize( 300 ) );
    }


    // -----------------

    TEST( SimpleRecordStoreV1, FullSimple1 ) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn,
                                "test.foo",
                                md,
                                &em,
                                false );


        ASSERT_EQUALS( 0, md->numRecords() );
        StatusWith<DiskLoc> result = rs.insertRecord( &txn, "abc", 4, 1000 );
        ASSERT_TRUE( result.isOK() );
        ASSERT_EQUALS( 1, md->numRecords() );
        Record* record = rs.recordFor( result.getValue() );
        ASSERT_EQUALS( string("abc"), string(record->data()) );
    }

    // ----------------

    /**
     * Inserts take the first deleted record with the correct size.
     */
    TEST( SimpleRecordStoreV1, InsertTakesFirstDeletedWithExactSize ) {
        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 100},
                {DiskLoc(0, 1100), 100},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(2, 1100), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1200), 100}, // this one will be used
                {DiskLoc(2, 1000), 100},
                {DiskLoc(1, 1000), 1000},
                {}
            };

            initializeV1RS(&txn, recs, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 100 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1000), 100},
                {DiskLoc(0, 1100), 100},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1200), 100}, // this is the new record
                {DiskLoc(2, 1100), 100},
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(2, 1000), 100},
                {DiskLoc(1, 1000), 1000},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

}
