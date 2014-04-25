// namespacetests.cpp : namespace.{h,cpp} unit tests.
//

/**
 *    Copyright (C) 2008 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

// Where IndexDetails defined.
#include "mongo/pch.h"

#include "mongo/db/db.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/json.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/structure/record_store_v1_capped.h"
#include "mongo/db/structure/record_store_v1_simple.h"
#include "mongo/db/structure/catalog/namespace.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/catalog/namespace_details_rsv1_metadata.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/dbtests/dbtests.h"


namespace NamespaceTests {

    const int MinExtentSize = 4096;

    namespace MissingFieldTests {

        /** A missing field is represented as null in a btree index. */
        class BtreeIndexMissingField {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << 1 ) ));
                ASSERT_EQUALS(jstNULL, IndexLegacy::getMissingField(NULL,spec).firstElement().type());
            }
        };
        
        /** A missing field is represented as null in a 2d index. */
        class TwoDIndexMissingField {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << "2d" ) ));
                ASSERT_EQUALS(jstNULL, IndexLegacy::getMissingField(NULL,spec).firstElement().type());
            }
        };

        /** A missing field is represented with the hash of null in a hashed index. */
        class HashedIndexMissingField {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << "hashed" ) ));
                BSONObj nullObj = BSON( "a" << BSONNULL );

                // Call getKeys on the nullObj.
                BSONObjSet nullFieldKeySet;
                ExpressionKeysPrivate::getHashKeys(nullObj, "a", 0, 0, false, &nullFieldKeySet);
                BSONElement nullFieldFromKey = nullFieldKeySet.begin()->firstElement();

                ASSERT_EQUALS( ExpressionKeysPrivate::makeSingleHashKey( nullObj.firstElement(), 0, 0 ),
                               nullFieldFromKey.Long() );

                BSONObj missingField = IndexLegacy::getMissingField(NULL,spec);
                ASSERT_EQUALS( NumberLong, missingField.firstElement().type() );
                ASSERT_EQUALS( nullFieldFromKey, missingField.firstElement());
            }
        };

        /**
         * A missing field is represented with the hash of null in a hashed index.  This hash value
         * depends on the hash seed.
         */
        class HashedIndexMissingFieldAlternateSeed {
        public:
            void run() {
                BSONObj spec( BSON("key" << BSON( "a" << "hashed" ) <<  "seed" << 0x5eed ));
                BSONObj nullObj = BSON( "a" << BSONNULL );

                BSONObjSet nullFieldKeySet;
                ExpressionKeysPrivate::getHashKeys(nullObj, "a", 0x5eed, 0, false, &nullFieldKeySet);
                BSONElement nullFieldFromKey = nullFieldKeySet.begin()->firstElement();

                ASSERT_EQUALS( ExpressionKeysPrivate::makeSingleHashKey( nullObj.firstElement(), 0x5eed, 0 ),
                               nullFieldFromKey.Long() );

                // Ensure that getMissingField recognizes that the seed is different (and returns
                // the right key).
                BSONObj missingField = IndexLegacy::getMissingField(NULL,spec);
                ASSERT_EQUALS( NumberLong, missingField.firstElement().type());
                ASSERT_EQUALS( nullFieldFromKey, missingField.firstElement());
            }
        };
        
    } // namespace MissingFieldTests
    
    namespace NamespaceDetailsTests {

        class Base {
            const char *ns_;
            Lock::GlobalWrite lk;
            Client::Context _context;
        public:
            Base( const char *ns = "unittests.NamespaceDetailsTests" ) : ns_( ns ) , _context( ns ) {}
            virtual ~Base() {
                if ( !nsd() )
                    return;
                _context.db()->dropCollection( ns() );
            }
        protected:
            void create() {
                Lock::GlobalWrite lk;
                ASSERT( userCreateNS( db(), ns(), fromjson( spec() ), false ).isOK() );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":1}";
            }
            int nRecords() const {
                int count = 0;
                const Extent* ext;
                for ( DiskLoc extLoc = nsd()->firstExtent();
                        !extLoc.isNull();
                        extLoc = ext->xnext) {
                    ext = extentManager().getExtent(extLoc);
                    int fileNo = ext->firstRecord.a();
                    if ( fileNo == -1 )
                        continue;
                    for ( int recOfs = ext->firstRecord.getOfs(); recOfs != DiskLoc::NullOfs;
                          recOfs = recordStore()->recordFor(DiskLoc(fileNo, recOfs))->nextOfs() ) {
                        ++count;
                    }
                }
                ASSERT_EQUALS( count, nsd()->numRecords() );
                return count;
            }
            int nExtents() const {
                int count = 0;
                for ( DiskLoc extLoc = nsd()->firstExtent();
                        !extLoc.isNull();
                        extLoc = extentManager().getExtent(extLoc)->xnext ) {
                    ++count;
                }
                return count;
            }
            static int min( int a, int b ) {
                return a < b ? a : b;
            }
            const char *ns() const {
                return ns_;
            }
            NamespaceDetails *nsd() const {
                Collection* c = collection();
                if ( !c )
                    return NULL;
                return c->detailsWritable()->writingWithExtra();
            }
            const RecordStore* recordStore() const {
                Collection* c = collection();
                if ( !c )
                    return NULL;
                return c->getRecordStore();
            }
            const RecordStoreV1Base* cheatRecordStore() const {
                return dynamic_cast<const RecordStoreV1Base*>( recordStore() );
            }

            Database* db() const {
                return _context.db();
            }
            const ExtentManager& extentManager() const {
                return db()->getExtentManager();
            }
            Collection* collection() const {
                return db()->getCollection( ns() );
            }
            IndexCatalog* indexCatalog() const { 
                return collection()->getIndexCatalog();
            }
            CollectionInfoCache* infoCache() const {
                return collection()->infoCache();
            }

            static BSONObj bigObj() {
                BSONObjBuilder b;
                b.appendOID("_id", 0, true);
                string as( 187, 'a' );
                b.append( "a", as );
                return b.obj();
            }

            /** Return the smallest DeletedRecord in deletedList, or DiskLoc() if none. */
            DiskLoc smallestDeletedRecord() {
                for( int i = 0; i < Buckets; ++i ) {
                    if ( !nsd()->deletedListEntry( i ).isNull() ) {
                        return nsd()->deletedListEntry( i );
                    }
                }
                return DiskLoc();
            }

            /**
             * 'cook' the deletedList by shrinking the smallest deleted record to size
             * 'newDeletedRecordSize'.
             */
            void cookDeletedList( int newDeletedRecordSize ) {

                // Extract the first DeletedRecord from the deletedList.
                DiskLoc deleted;
                for( int i = 0; i < Buckets; ++i ) {
                    if ( !nsd()->deletedListEntry( i ).isNull() ) {
                        deleted = nsd()->deletedListEntry( i );
                        nsd()->deletedListEntry( i ).writing().Null();
                        break;
                    }
                }
                ASSERT( !deleted.isNull() );

                const RecordStore* rs = collection()->getRecordStore();

                // Shrink the DeletedRecord's size to newDeletedRecordSize.
                ASSERT_GREATER_THAN_OR_EQUALS( rs->deletedRecordFor( deleted )->lengthWithHeaders(),
                                               newDeletedRecordSize );
                DeletedRecord* dr = const_cast<DeletedRecord*>( rs->deletedRecordFor( deleted ) );
                getDur().writingInt( dr->lengthWithHeaders() ) = newDeletedRecordSize;

                // Re-insert the DeletedRecord into the deletedList bucket appropriate for its
                // new size.
                nsd()->deletedListEntry( NamespaceDetails::bucket( newDeletedRecordSize ) ).writing() =
                        deleted;
            }
        };

        class Create : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd() );
                ASSERT_EQUALS( 0, nRecords() );
                ASSERT( nsd()->firstExtent() == nsd()->capExtent() );
                DiskLoc initial = DiskLoc();
                initial.setInvalid();
                ASSERT( initial == nsd()->capFirstNewRecord() );
            }
        };

        class SingleAlloc : public Base {
        public:
            void run() {
                create();
                BSONObj b = bigObj();
                ASSERT( collection()->insertDocument( b, true ).isOK() );
                ASSERT_EQUALS( 1, nRecords() );
            }
        };

        class Realloc : public Base {
        public:
            void run() {
                create();

                const int N = 20;
                const int Q = 16; // these constants depend on the size of the bson object, the extent size allocated by the system too
                DiskLoc l[ N ];
                for ( int i = 0; i < N; ++i ) {
                    BSONObj b = bigObj();
                    StatusWith<DiskLoc> status = collection()->insertDocument( b, true );
                    ASSERT( status.isOK() );
                    l[ i ] = status.getValue();
                    ASSERT( !l[ i ].isNull() );
                    ASSERT( nRecords() <= Q );
                    //ASSERT_EQUALS( 1 + i % 2, nRecords() );
                    if ( i >= 16 )
                        ASSERT( l[ i ] == l[ i - Q] );
                }
            }
        };

        class TwoExtent : public Base {
        public:
            void run() {
                create();
                ASSERT_EQUALS( 2, nExtents() );

                DiskLoc l[ 8 ];
                for ( int i = 0; i < 8; ++i ) {
                    StatusWith<DiskLoc> status = collection()->insertDocument( bigObj(), true );
                    ASSERT( status.isOK() );
                    l[ i ] = status.getValue();
                    ASSERT( !l[ i ].isNull() );
                    //ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                    //if ( i > 3 )
                    //    ASSERT( l[ i ] == l[ i - 4 ] );
                }
                ASSERT( nRecords() == 8 );

                // Too big
                BSONObjBuilder bob;
                bob.appendOID( "_id", NULL, true );
                bob.append( "a", string( MinExtentSize + 500, 'a' ) ); // min extent size is now 4096
                BSONObj bigger = bob.done();
                StatusWith<DiskLoc> status = collection()->insertDocument( bigger, false );
                ASSERT( !status.isOK() );
                ASSERT_EQUALS( 0, nRecords() );
            }
        private:
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
            }
        };


        /**
         * Test  Quantize record allocation size for various buckets
         *       @see NamespaceDetails::quantizeAllocationSpace()
         */
        class QuantizeFixedBuckets : public Base {
        public:
            void run() {
                create();
                // explicitly test for a set of known values
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(33),       36);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(1000),     1024);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(10001),    10240);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(100000),   106496);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(1000001),  1048576);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(10000000), 10223616);
            }
        };


        /**
         * Test  Quantize min/max record allocation size
         *       @see NamespaceDetails::quantizeAllocationSpace()
         */
        class QuantizeMinMaxBound : public Base {
        public:
            void run() {
                create();
                // test upper and lower bound
                const int maxSize = 16 * 1024 * 1024;
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(1), 2);
                ASSERT_EQUALS(NamespaceDetails::quantizeAllocationSpace(maxSize), maxSize);
            }
        };

        /**
         * Test  Quantize record allocation on every boundary, as well as boundary-1
         *       @see NamespaceDetails::quantizeAllocationSpace()
         */
        class QuantizeRecordBoundary : public Base {
        public:
            void run() {
                create();
                for (int iBucket = 0; iBucket <= MaxBucket; ++iBucket) {
                    // for each bucket in range [min, max)
                    const int bucketSize = bucketSizes[iBucket];
                    const int prevBucketSize = (iBucket - 1 >= 0) ? bucketSizes[iBucket - 1] : 0;
                    const int intervalSize = bucketSize / 16;
                    for (int iBoundary = prevBucketSize;
                         iBoundary < bucketSize;
                         iBoundary += intervalSize) {
                        // for each quantization boundary within the bucket
                        for (int iSize = iBoundary - 1; iSize <= iBoundary; ++iSize) {
                            // test the quantization boundary - 1, and the boundary itself
                            const int quantized =
                                    NamespaceDetails::quantizeAllocationSpace(iSize);
                            // assert quantized size is greater than or equal to requested size
                            ASSERT(quantized >= iSize);
                            // assert quantized size is within one quantization interval of
                            // the requested size
                            ASSERT(quantized - iSize <= intervalSize);
                            // assert quantization is an idempotent operation
                            ASSERT(quantized ==
                                   NamespaceDetails::quantizeAllocationSpace(quantized));
                        }
                    }
                }
            }
        };

        /**
         * Except for the largest bucket, quantizePowerOf2AllocationSpace quantizes to the nearest
         * bucket size.
         */
        class QuantizePowerOf2ToBucketSize : public Base {
        public:
            void run() {
                create();
                for( int iBucket = 0; iBucket < MaxBucket - 1; ++iBucket ) {
                    int bucketSize = bucketSizes[ iBucket ];
                    int nextBucketSize = bucketSizes[ iBucket + 1 ];

                    // bucketSize - 1 is quantized to bucketSize.
                    ASSERT_EQUALS( bucketSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace
                                           ( bucketSize - 1 ) );

                    // bucketSize is quantized to nextBucketSize.
                    // Descriptive rather than normative test.
                    // SERVER-8311 A pre quantized size is rounded to the next quantum level.
                    ASSERT_EQUALS( nextBucketSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace
                                           ( bucketSize ) );

                    // bucketSize + 1 is quantized to nextBucketSize.
                    ASSERT_EQUALS( nextBucketSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace
                                           ( bucketSize + 1 ) );
                }

                // The next to largest bucket size - 1 is quantized to the next to largest bucket
                // size.
                ASSERT_EQUALS( bucketSizes[ MaxBucket - 1 ],
                               NamespaceDetails::quantizePowerOf2AllocationSpace
                                       ( bucketSizes[ MaxBucket - 1 ] - 1 ) );
            }
        };

        /**
         * Within the largest bucket, quantizePowerOf2AllocationSpace quantizes to the nearest
         * megabyte boundary.
         */
        class QuantizeLargePowerOf2ToMegabyteBoundary : public Base {
        public:
            void run() {
                create();
                
                // Iterate iSize over all 1mb boundaries from the size of the next to largest bucket
                // to the size of the largest bucket + 1mb.
                for( int iSize = bucketSizes[ MaxBucket - 1 ];
                     iSize <= bucketSizes[ MaxBucket ] + 0x100000;
                     iSize += 0x100000 ) {

                    // iSize - 1 is quantized to iSize.
                    ASSERT_EQUALS( iSize,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace( iSize - 1 ) );

                    // iSize is quantized to iSize + 1mb.
                    // Descriptive rather than normative test.
                    // SERVER-8311 A pre quantized size is rounded to the next quantum level.
                    ASSERT_EQUALS( iSize + 0x100000,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace( iSize ) );

                    // iSize + 1 is quantized to iSize + 1mb.
                    ASSERT_EQUALS( iSize + 0x100000,
                                   NamespaceDetails::quantizePowerOf2AllocationSpace( iSize + 1 ) );
                }
            }
        };

        /** getRecordAllocationSize() returns its argument when the padding factor is 1.0. */
        class GetRecordAllocationSizeNoPadding : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd()->clearUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                ASSERT_EQUALS( 1.0, nsd()->paddingFactor() );
                ASSERT_EQUALS( 300, cheatRecordStore()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        /** getRecordAllocationSize() multiplies by a padding factor > 1.0. */
        class GetRecordAllocationSizeWithPadding : public Base {
        public:
            void run() {
                create();
                double paddingFactor = 1.2;
                nsd()->setPaddingFactor( paddingFactor );
                ASSERT( nsd()->clearUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                ASSERT_EQUALS( paddingFactor, nsd()->paddingFactor() );
                ASSERT_EQUALS( static_cast<int>( 300 * paddingFactor ),
                               cheatRecordStore()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        /**
         * getRecordAllocationSize() quantizes to the nearest power of 2 when Flag_UsePowerOf2Sizes
         * is set.
         */
        class GetRecordAllocationSizePowerOf2 : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd()->isUserFlagSet( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                ASSERT_EQUALS( 512, cheatRecordStore()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        
        /**
         * getRecordAllocationSize() quantizes to the nearest power of 2 when Flag_UsePowerOf2Sizes
         * is set, ignoring the padding factor.
         */
        class GetRecordAllocationSizePowerOf2PaddingIgnored : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd()->isUserFlagSet( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                nsd()->setPaddingFactor( 2.0 );
                ASSERT_EQUALS( 2.0, nsd()->paddingFactor() );
                ASSERT_EQUALS( 512, cheatRecordStore()->getRecordAllocationSize( 300 ) );
            }
            virtual string spec() const { return ""; }
        };

        BSONObj docForRecordSize( int size ) {
            BSONObjBuilder b;
            b.append( "_id", 5 );
            b.append( "x", string( size - Record::HeaderSize - 22, 'x' ) );
            BSONObj x = b.obj();
            ASSERT_EQUALS( Record::HeaderSize + x.objsize(), size );
            return x;
        }

        /** alloc() quantizes the requested size using quantizeAllocationSpace() rules. */
        class AllocQuantized : public Base {
        public:
            void run() {

                string myns = (string)ns() + "AllocQuantized";
                db()->namespaceIndex().add_ns( myns, DiskLoc(), false );
                SimpleRecordStoreV1 rs( myns,
                                        new NamespaceDetailsRSV1MetaData( db()->namespaceIndex().details( myns ) ),
                                        &db()->getExtentManager(),
                                        false );

                BSONObj obj = docForRecordSize( 300 );
                StatusWith<DiskLoc> result = rs.insertRecord( obj.objdata(), obj.objsize(), 0 );
                ASSERT( result.isOK() );

                // The length of the allocated record is quantized.
                ASSERT_EQUALS( 320, rs.recordFor( result.getValue() )->lengthWithHeaders() );
            }
            virtual string spec() const { return ""; }
        };

        /** alloc() does not quantize records in capped collections. */
        class AllocCappedNotQuantized : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd()->isCapped() );
                ASSERT( !nsd()->isUserFlagSet( NamespaceDetails::Flag_UsePowerOf2Sizes ) );

                StatusWith<DiskLoc> result =
                    collection()->insertDocument( docForRecordSize( 300 ), false );
                ASSERT( result.isOK() );
                Record* record = collection()->getRecordStore()->recordFor( result.getValue() );
                // Check that no quantization is performed.
                ASSERT_EQUALS( 300, record->lengthWithHeaders() );
            }
            virtual string spec() const { return "{capped:true,size:2048}"; }
        };

        /**
         * alloc() does not quantize records in index collections using quantizeAllocationSpace()
         * rules.
         */
        class AllocIndexNamespaceNotQuantized : public Base {
        public:
            void run() {
                string myns = (string)ns() + "AllocIndexNamespaceNotQuantized";

                db()->namespaceIndex().add_ns( myns, DiskLoc(), false );
                SimpleRecordStoreV1 rs( myns + ".$x",
                                        new NamespaceDetailsRSV1MetaData( db()->namespaceIndex().details( myns ) ),
                                        &db()->getExtentManager(),
                                        false );

                BSONObj obj = docForRecordSize( 300 );
                StatusWith<DiskLoc> result = rs.insertRecord( obj.objdata(), obj.objsize(), 0 );
                ASSERT( result.isOK() );

                // The length of the allocated record is not quantized.
                ASSERT_EQUALS( 300, rs.recordFor( result.getValue() )->lengthWithHeaders() );

            }
        };

        /** alloc() quantizes records in index collections to the nearest multiple of 4. */
        class AllocIndexNamespaceSlightlyQuantized : public Base {
        public:
            void run() {
                string myns = (string)ns() + "AllocIndexNamespaceNotQuantized";

                db()->namespaceIndex().add_ns( myns, DiskLoc(), false );
                SimpleRecordStoreV1 rs( myns + ".$x",
                                        new NamespaceDetailsRSV1MetaData( db()->namespaceIndex().details( myns ) ),
                                        &db()->getExtentManager(),
                                        true );

                BSONObj obj = docForRecordSize( 298 );
                StatusWith<DiskLoc> result = rs.insertRecord( obj.objdata(), obj.objsize(), 0 );
                ASSERT( result.isOK() );

                ASSERT_EQUALS( 300, rs.recordFor( result.getValue() )->lengthWithHeaders() );
            }
        };

        /** alloc() returns a non quantized record larger than the requested size. */
        class AllocUseNonQuantizedDeletedRecord : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 310 );

                StatusWith<DiskLoc> actualLocation = collection()->insertDocument( docForRecordSize(300),
                                                                                   false );
                ASSERT( actualLocation.isOK() );
                Record* rec = collection()->getRecordStore()->recordFor( actualLocation.getValue() );
                ASSERT_EQUALS( 310, rec->lengthWithHeaders() );

                // No deleted records remain after alloc returns the non quantized record.
                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return "{ flags : 0 }"; }
        };

        /** alloc() returns a non quantized record equal to the requested size. */
        class AllocExactSizeNonQuantizedDeletedRecord : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 300 );

                StatusWith<DiskLoc> actualLocation = collection()->insertDocument( docForRecordSize(300),
                                                                                   false );
                ASSERT( actualLocation.isOK() );
                Record* rec = collection()->getRecordStore()->recordFor( actualLocation.getValue() );
                ASSERT_EQUALS( 300, rec->lengthWithHeaders() );

                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return "{ flags : 0 }"; }
        };

        /**
         * alloc() returns a non quantized record equal to the quantized size plus some extra space
         * too small to make a DeletedRecord.
         */
        class AllocQuantizedWithExtra : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 343 );

                StatusWith<DiskLoc> actualLocation = collection()->insertDocument( docForRecordSize(300),
                                                                                   false );
                ASSERT( actualLocation.isOK() );
                Record* rec = collection()->getRecordStore()->recordFor( actualLocation.getValue() );
                ASSERT_EQUALS( 343, rec->lengthWithHeaders() );

                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return "{ flags : 0 }"; }
        };

        /**
         * alloc() returns a quantized record when the extra space in the reclaimed deleted record
         * is large enough to form a new deleted record.
         */
        class AllocQuantizedWithoutExtra : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 344 );

                const RecordStore* rs = collection()->getRecordStore();

                // The returned record is quantized from 300 to 320.
                StatusWith<DiskLoc> actualLocation = collection()->insertDocument( docForRecordSize(300),
                                                                                   false );
                ASSERT( actualLocation.isOK() );
                Record* rec = rs->recordFor( actualLocation.getValue() );
                ASSERT_EQUALS( 320, rec->lengthWithHeaders() );

                // A new 24 byte deleted record is split off.
                ASSERT_EQUALS( 24,
                               rs->deletedRecordFor(smallestDeletedRecord())->lengthWithHeaders() );
            }
            virtual string spec() const { return "{ flags : 0 }"; }
        };

        /**
         * A non quantized deleted record within 1/8 of the requested size is returned as is, even
         * if a quantized portion of the deleted record could be used instead.
         */
        class AllocNotQuantizedNearDeletedSize : public Base {
        public:
            void run() {
                create();
                cookDeletedList( 344 );

                StatusWith<DiskLoc> actualLocation = collection()->insertDocument( docForRecordSize(319),
                                                                                   false );
                ASSERT( actualLocation.isOK() );
                Record* rec = collection()->getRecordStore()->recordFor( actualLocation.getValue() );

                // Even though 319 would be quantized to 320 and 344 - 320 == 24 could become a new
                // deleted record, the entire deleted record is returned because
                // ( 344 - 320 ) < ( 320 >> 3 ).
                ASSERT_EQUALS( 344, rec->lengthWithHeaders() );
                ASSERT_EQUALS( DiskLoc(), smallestDeletedRecord() );
            }
            virtual string spec() const { return "{ flags : 0 }"; }
        };

        /* test  NamespaceDetails::cappedTruncateAfter(const char *ns, DiskLoc loc)
        */
        class TruncateCapped : public Base {
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
            }
            void pass(int p) {
                create();
                ASSERT_EQUALS( 2, nExtents() );

                BSONObj b = bigObj();

                int N = MinExtentSize / b.objsize() * nExtents() + 5;
                int T = N - 4;

                DiskLoc truncAt;
                //DiskLoc l[ 8 ];
                for ( int i = 0; i < N; ++i ) {
                    BSONObj bb = bigObj();
                    StatusWith<DiskLoc> status = collection()->insertDocument( bb, true );
                    ASSERT( status.isOK() );
                    DiskLoc a = status.getValue();
                    if( T == i )
                        truncAt = a;
                    ASSERT( !a.isNull() );
                    /*ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                    if ( i > 3 )
                        ASSERT( l[ i ] == l[ i - 4 ] );*/
                }
                ASSERT( nRecords() < N );

                DiskLoc last, first;
                {
                    auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns(),
                                                                            collection(),
                                                                            InternalPlanner::BACKWARD));
                    runner->getNext(NULL, &last);
                    ASSERT( !last.isNull() );
                }
                {
                    auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns(),
                                                                            collection(),
                                                                            InternalPlanner::FORWARD));
                    runner->getNext(NULL, &first);
                    ASSERT( !first.isNull() );
                    ASSERT( first != last ) ;
                }

                collection()->temp_cappedTruncateAfter(truncAt, false);
                ASSERT_EQUALS( collection()->numRecords() , 28u );

                {
                    DiskLoc loc;
                    auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns(),
                                                                            collection(),
                                                                            InternalPlanner::FORWARD));
                    runner->getNext(NULL, &loc);
                    ASSERT( first == loc);
                }
                {
                    auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns(),
                                                                            collection(),
                                                                            InternalPlanner::BACKWARD));
                    DiskLoc loc;
                    runner->getNext(NULL, &loc);
                    ASSERT( last != loc );
                    ASSERT( !last.isNull() );
                }

                // Too big
                BSONObjBuilder bob;
                bob.appendOID("_id", 0, true);
                bob.append( "a", string( MinExtentSize + 300, 'a' ) );
                BSONObj bigger = bob.done();
                StatusWith<DiskLoc> status = collection()->insertDocument( bigger, true );
                ASSERT( !status.isOK() );
                ASSERT_EQUALS( 0, nRecords() );
            }
        public:
            void run() {
//                log() << "******** NOT RUNNING TruncateCapped test yet ************" << endl;
                pass(0);
            }
        };

#if 0 // XXXXXX - once RecordStore is clean, we can put this back
        class Migrate : public Base {
        public:
            void run() {
                create();
                nsd()->deletedListEntry( 2 ) = nsd()->cappedListOfAllDeletedRecords().drec()->nextDeleted().drec()->nextDeleted();
                nsd()->cappedListOfAllDeletedRecords().drec()->nextDeleted().drec()->nextDeleted().writing() = DiskLoc();
                nsd()->cappedLastDelRecLastExtent().Null();
                NamespaceDetails *d = nsd();

                zero( &d->capExtent() );
                zero( &d->capFirstNewRecord() );

                // this has a side effect of called NamespaceDetails::cappedCheckMigrate
                db()->namespaceIndex().details( ns() );

                ASSERT( nsd()->firstExtent() == nsd()->capExtent() );
                ASSERT( nsd()->capExtent().getOfs() != 0 );
                ASSERT( !nsd()->capFirstNewRecord().isValid() );
                int nDeleted = 0;
                for ( DiskLoc i = nsd()->cappedListOfAllDeletedRecords(); !i.isNull(); i = i.drec()->nextDeleted(), ++nDeleted );
                ASSERT_EQUALS( 10, nDeleted );
                ASSERT( nsd()->cappedLastDelRecLastExtent().isNull() );
            }
        private:
            static void zero( DiskLoc *d ) {
                memset( d, 0, sizeof( DiskLoc ) );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":10}";
            }
        };
#endif

        // This isn't a particularly useful test, and because it doesn't clean up
        // after itself, /tmp/unittest needs to be cleared after running.
        //        class BigCollection : public Base {
        //        public:
        //            BigCollection() : Base( "NamespaceDetailsTests_BigCollection" ) {}
        //            void run() {
        //                create();
        //                ASSERT_EQUALS( 2, nExtents() );
        //            }
        //        private:
        //            virtual string spec() const {
        //                // NOTE 256 added to size in _userCreateNS()
        //                long long big = DataFile::maxSize() - DataFileHeader::HeaderSize;
        //                stringstream ss;
        //                ss << "{\"capped\":true,\"size\":" << big << "}";
        //                return ss.str();
        //            }
        //        };

        class Size {
        public:
            void run() {
                ASSERT_EQUALS( 496U, sizeof( NamespaceDetails ) );
            }
        };
        
        class SwapIndexEntriesTest : public Base {
        public:
            void run() {
                create();
                NamespaceDetails *nsd = collection()->detailsWritable();

                // Set 2 & 54 as multikey
                nsd->setIndexIsMultikey(2, true);
                nsd->setIndexIsMultikey(54, true);
                ASSERT(nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(54));

                // Flip 2 & 47
                nsd->setIndexIsMultikey(2, false);
                nsd->setIndexIsMultikey(47, true);
                ASSERT(!nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(47));

                // Reset entries that are already true
                nsd->setIndexIsMultikey(54, true);
                nsd->setIndexIsMultikey(47, true);
                ASSERT(nsd->isMultikey(54));
                ASSERT(nsd->isMultikey(47));

                // Two non-multi-key
                nsd->setIndexIsMultikey(2, false);
                nsd->setIndexIsMultikey(43, false);
                ASSERT(!nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(54));
                ASSERT(nsd->isMultikey(47));
                ASSERT(!nsd->isMultikey(43));
            }
        };

    } // namespace NamespaceDetailsTests

    class All : public Suite {
    public:
        All() : Suite( "namespace" ) {
        }

        void setupTests() {
            add< NamespaceDetailsTests::Create >();
            add< NamespaceDetailsTests::SingleAlloc >();
            add< NamespaceDetailsTests::Realloc >();
            add< NamespaceDetailsTests::QuantizeMinMaxBound >();
            add< NamespaceDetailsTests::QuantizeFixedBuckets >();
            add< NamespaceDetailsTests::QuantizeRecordBoundary >();
            add< NamespaceDetailsTests::QuantizePowerOf2ToBucketSize >();
            add< NamespaceDetailsTests::QuantizeLargePowerOf2ToMegabyteBoundary >();
            add< NamespaceDetailsTests::GetRecordAllocationSizeNoPadding >();
            add< NamespaceDetailsTests::GetRecordAllocationSizeWithPadding >();
            add< NamespaceDetailsTests::GetRecordAllocationSizePowerOf2 >();
            add< NamespaceDetailsTests::GetRecordAllocationSizePowerOf2PaddingIgnored >();
            add< NamespaceDetailsTests::AllocQuantized >();
            add< NamespaceDetailsTests::AllocCappedNotQuantized >();
            add< NamespaceDetailsTests::AllocIndexNamespaceNotQuantized >();
            add< NamespaceDetailsTests::AllocIndexNamespaceSlightlyQuantized >();
            add< NamespaceDetailsTests::AllocUseNonQuantizedDeletedRecord >();
            add< NamespaceDetailsTests::AllocExactSizeNonQuantizedDeletedRecord >();
            add< NamespaceDetailsTests::AllocQuantizedWithExtra >();
            add< NamespaceDetailsTests::AllocQuantizedWithoutExtra >();
            add< NamespaceDetailsTests::AllocNotQuantizedNearDeletedSize >();
            add< NamespaceDetailsTests::TwoExtent >();
            add< NamespaceDetailsTests::TruncateCapped >();
            //add< NamespaceDetailsTests::Migrate >();
            add< NamespaceDetailsTests::SwapIndexEntriesTest >();
            //            add< NamespaceDetailsTests::BigCollection >();
            add< NamespaceDetailsTests::Size >();
            add< MissingFieldTests::BtreeIndexMissingField >();
            add< MissingFieldTests::TwoDIndexMissingField >();
            add< MissingFieldTests::HashedIndexMissingField >();
            add< MissingFieldTests::HashedIndexMissingFieldAlternateSeed >();
        }
    } myall;
} // namespace NamespaceTests

