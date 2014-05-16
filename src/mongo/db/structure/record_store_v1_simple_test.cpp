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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/record_store_v1_test_help.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    // Provides data to be inserted. Must be large enough for largest possible record.
    // Should be in BSS so unused portions should be free.
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );
        ASSERT_EQUALS( 1.0, md->paddingFactor() );
        ASSERT_EQUALS( 300, rs.getRecordAllocationSize( 300 ) );
    }

    /** getRecordAllocationSize() multiplies by a padding factor > 1.0. */
    TEST(SimpleRecordStoreV1, GetRecordAllocationSizeWithPadding) {
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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
        OperationContextNoop txn;
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

    /**
     * Test that we keep looking for better matches for 5 links once we find a non-exact match.
     * This "extra" scanning does not proceed into bigger buckets.
     * WARNING: this test depends on magic numbers inside RSV1Simple::_allocFromExistingExtents.
     */
    TEST( SimpleRecordStoreV1, InsertLooksForBetterMatchUpTo5Links ) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize recs[] = {
                {}
            };
            LocAndSize drecs[] = {
                // This intentionally leaves gaps to keep locs readable.
                {DiskLoc(0, 1000),  75}, // too small
                {DiskLoc(0, 1100), 100}, // 1st big enough: will be first record
                {DiskLoc(0, 1200), 100}, // 2nd: will be third record
                {DiskLoc(0, 1300), 100}, // 3rd
                {DiskLoc(0, 1400), 100}, // 4th
                {DiskLoc(0, 1500), 100}, // 5th: first and third will stop once they look here
                {DiskLoc(0, 1600),  80}, // 6th: second will make it here and use this
                {DiskLoc(0, 1700), 999}, // bigger bucket. Should never look here
                {}
            };
            initializeV1RS(&txn, recs, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 1100), 100}, // 1st insert
                {DiskLoc(0, 1600),  80}, // 2nd insert
                {DiskLoc(0, 1200), 100}, // 3rd insert
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000),  75},
                {DiskLoc(0, 1300), 100},
                {DiskLoc(0, 1400), 100},
                {DiskLoc(0, 1500), 100},
                {DiskLoc(0, 1700), 999},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

    /**
     * Test that we stop looking in a bucket once we see 31 too small drecs.
     * WARNING: this test depends on magic numbers inside RSV1Simple::_allocFromExistingExtents.
     */
    TEST( SimpleRecordStoreV1, InsertLooksForMatchUpTo31Links ) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize recs[] = {
                {}
            };
            LocAndSize drecs[] = {
                // This intentionally leaves gaps to keep locs readable.
                {DiskLoc(0, 1000),  50}, // different bucket

                {DiskLoc(0, 1100),  75}, // 1st too small in correct bucket
                {DiskLoc(0, 1200),  75},
                {DiskLoc(0, 1300),  75},
                {DiskLoc(0, 1400),  75},
                {DiskLoc(0, 1500),  75},
                {DiskLoc(0, 1600),  75},
                {DiskLoc(0, 1700),  75},
                {DiskLoc(0, 1800),  75},
                {DiskLoc(0, 1900),  75},
                {DiskLoc(0, 2000),  75}, // 10th too small
                {DiskLoc(0, 2100),  75},
                {DiskLoc(0, 2200),  75},
                {DiskLoc(0, 2300),  75},
                {DiskLoc(0, 2400),  75},
                {DiskLoc(0, 2500),  75},
                {DiskLoc(0, 2600),  75},
                {DiskLoc(0, 2700),  75},
                {DiskLoc(0, 2800),  75},
                {DiskLoc(0, 2900),  75},
                {DiskLoc(0, 3000),  75}, // 20th too small
                {DiskLoc(0, 3100),  75},
                {DiskLoc(0, 3200),  75},
                {DiskLoc(0, 3300),  75},
                {DiskLoc(0, 3400),  75},
                {DiskLoc(0, 3500),  75},
                {DiskLoc(0, 3600),  75},
                {DiskLoc(0, 3700),  75},
                {DiskLoc(0, 3800),  75},
                {DiskLoc(0, 3900),  75},
                {DiskLoc(0, 4000),  75}, // 30th too small
                {DiskLoc(0, 4100),  75}, // 31st too small

                {DiskLoc(0, 8000),  80}, // big enough but wont be seen until we take an earlier one
                {DiskLoc(0, 9000), 140}, // bigger bucket. jumps here after seeing 31 drecs
                {}
            };
            initializeV1RS(&txn, recs, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0); // takes from bigger bucket
        rs.insertRecord(&txn, zeros, 70 - Record::HeaderSize, 0); // removes a 75-sized drec
        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0); // now sees big-enough drec

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 9000), 80}, // 1st insert went here
                {DiskLoc(0, 1100), 75}, // 2nd here
                {DiskLoc(0, 8000), 80}, // 3rd here
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 9000 + 80),  140 - 80}, // split off during first insert
                {DiskLoc(0, 1000),  50},
                {DiskLoc(0, 1200),  75},
                {DiskLoc(0, 1300),  75},
                {DiskLoc(0, 1400),  75},
                {DiskLoc(0, 1500),  75},
                {DiskLoc(0, 1600),  75},
                {DiskLoc(0, 1700),  75},
                {DiskLoc(0, 1800),  75},
                {DiskLoc(0, 1900),  75},
                {DiskLoc(0, 2000),  75},
                {DiskLoc(0, 2100),  75},
                {DiskLoc(0, 2200),  75},
                {DiskLoc(0, 2300),  75},
                {DiskLoc(0, 2400),  75},
                {DiskLoc(0, 2500),  75},
                {DiskLoc(0, 2600),  75},
                {DiskLoc(0, 2700),  75},
                {DiskLoc(0, 2800),  75},
                {DiskLoc(0, 2900),  75},
                {DiskLoc(0, 3000),  75},
                {DiskLoc(0, 3100),  75},
                {DiskLoc(0, 3200),  75},
                {DiskLoc(0, 3300),  75},
                {DiskLoc(0, 3400),  75},
                {DiskLoc(0, 3500),  75},
                {DiskLoc(0, 3600),  75},
                {DiskLoc(0, 3700),  75},
                {DiskLoc(0, 3800),  75},
                {DiskLoc(0, 3900),  75},
                {DiskLoc(0, 4000),  75},
                {DiskLoc(0, 4100),  75},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }

    /**
     * Test that we stop looking in a bucket once we see 31 drecs, or look 4-past the first
     * too-large match, whichever comes first. This is a combination of
     * InsertLooksForBetterMatchUpTo5Links and InsertLooksForMatchUpTo31Links.
     *
     * WARNING: this test depends on magic numbers inside RSV1Simple::_allocFromExistingExtents.
     */
    TEST( SimpleRecordStoreV1, InsertLooksForMatchUpTo31LinksEvenIfFoundOversizedFit ) {
        OperationContextNoop txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( false, 0 );
        SimpleRecordStoreV1 rs( &txn, "test.foo", md, &em, false );

        {
            LocAndSize recs[] = {
                {}
            };
            LocAndSize drecs[] = {
                // This intentionally leaves gaps to keep locs readable.
                {DiskLoc(0, 1000),  50}, // different bucket

                {DiskLoc(0, 1100),  75}, // 1st too small in correct bucket
                {DiskLoc(0, 1200),  75},
                {DiskLoc(0, 1300),  75},
                {DiskLoc(0, 1400),  75},
                {DiskLoc(0, 1500),  75},
                {DiskLoc(0, 1600),  75},
                {DiskLoc(0, 1700),  75},
                {DiskLoc(0, 1800),  75},
                {DiskLoc(0, 1900),  75},
                {DiskLoc(0, 2000),  75}, // 10th too small
                {DiskLoc(0, 2100),  75},
                {DiskLoc(0, 2200),  75},
                {DiskLoc(0, 2300),  75},
                {DiskLoc(0, 2400),  75},
                {DiskLoc(0, 2500),  75},
                {DiskLoc(0, 2600),  75},
                {DiskLoc(0, 2700),  75},
                {DiskLoc(0, 2800),  75},
                {DiskLoc(0, 2900),  75},
                {DiskLoc(0, 3000),  75}, // 20th too small
                {DiskLoc(0, 3100),  75},
                {DiskLoc(0, 3200),  75},
                {DiskLoc(0, 3300),  75},
                {DiskLoc(0, 3400),  75},
                {DiskLoc(0, 3500),  75},
                {DiskLoc(0, 3600),  75},
                {DiskLoc(0, 3700),  75}, // 27th too small

                {DiskLoc(0, 7000),  95}, // 1st insert takes this
                {DiskLoc(0, 7100),  95}, // 3rd insert takes this

                {DiskLoc(0, 3800),  75},
                {DiskLoc(0, 3900),  75}, // 29th too small (31st overall)

                {DiskLoc(0, 8000),  80}, // exact match. taken by 2nd insert

                {DiskLoc(0, 9000), 140}, // bigger bucket. Should never get here
                {}
            };
            initializeV1RS(&txn, recs, drecs, &em, md);
        }

        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0);
        rs.insertRecord(&txn, zeros, 80 - Record::HeaderSize, 0);

        {
            LocAndSize recs[] = {
                {DiskLoc(0, 7000), 95}, // 1st insert went here
                {DiskLoc(0, 8000), 80}, // 2nd here
                {DiskLoc(0, 7100), 95}, // 3rd here
                {}
            };
            LocAndSize drecs[] = {
                {DiskLoc(0, 1000),  50},
                {DiskLoc(0, 1100),  75},
                {DiskLoc(0, 1200),  75},
                {DiskLoc(0, 1300),  75},
                {DiskLoc(0, 1400),  75},
                {DiskLoc(0, 1500),  75},
                {DiskLoc(0, 1600),  75},
                {DiskLoc(0, 1700),  75},
                {DiskLoc(0, 1800),  75},
                {DiskLoc(0, 1900),  75},
                {DiskLoc(0, 2000),  75},
                {DiskLoc(0, 2100),  75},
                {DiskLoc(0, 2200),  75},
                {DiskLoc(0, 2300),  75},
                {DiskLoc(0, 2400),  75},
                {DiskLoc(0, 2500),  75},
                {DiskLoc(0, 2600),  75},
                {DiskLoc(0, 2700),  75},
                {DiskLoc(0, 2800),  75},
                {DiskLoc(0, 2900),  75},
                {DiskLoc(0, 3000),  75},
                {DiskLoc(0, 3100),  75},
                {DiskLoc(0, 3200),  75},
                {DiskLoc(0, 3300),  75},
                {DiskLoc(0, 3400),  75},
                {DiskLoc(0, 3500),  75},
                {DiskLoc(0, 3600),  75},
                {DiskLoc(0, 3700),  75},
                {DiskLoc(0, 3800),  75},
                {DiskLoc(0, 3900),  75},
                {DiskLoc(0, 9000), 140},
                {}
            };
            assertStateV1RS(recs, drecs, &em, md);
        }
    }
}
