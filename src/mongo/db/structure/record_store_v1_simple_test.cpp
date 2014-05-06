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

#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

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

            // iSize is quantized to iSize + 1mb.
            // Descriptive rather than normative test.
            // SERVER-8311 A pre quantized size is rounded to the next quantum level.
            ASSERT_EQUALS( iSize + 0x100000,
                           RecordStoreV1Base::quantizePowerOf2AllocationSpace( iSize ) );

            // iSize + 1 is quantized to iSize + 1mb.
            ASSERT_EQUALS( iSize + 0x100000,
                           RecordStoreV1Base::quantizePowerOf2AllocationSpace( iSize + 1 ) );
        }
    }

}
