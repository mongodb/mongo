/**
 *    Copyright (C) 2012 10gen Inc.
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
 */

#include "mongo/db/btreeposition.h"

#include "mongo/db/btree.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/pdfile.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/cstdint.h"

namespace BtreePositionTests {

    DBDirectClient _client;
    const char* _ns = "unittests.btreeposition";

    namespace BtreeKeyLocation {
        using mongo::BtreeKeyLocation;

        /** Check equality comparison performed by BtreeKeyLocation::operator==. */
        class Equality {
        public:
            void run() {
                // Equal initially.
                BtreeKeyLocation one, two;
                ASSERT_EQUALS( one, two );

                // Unequal with equal indexes but unequal buckets.
                one.bucket = DiskLoc( 1, 2 );
                ASSERT( !( one == two ) );

                // Unequal with equal buckets but unequal indexes.
                one.pos = 1;
                two.bucket = DiskLoc( 1, 2 );
                ASSERT( !( one == two ) );

                // Equal with both bucket and index equal.
                two.pos = 1;
                ASSERT_EQUALS( one, two );
            }
        };

    } // namespace BtreeKeyLocation

    namespace LogicalBtreePosition {
        using mongo::LogicalBtreePosition;
        using mongo::BtreeKeyLocation;

        /** Helper to construct custom btrees for tests. */
        class TestableBtree : public BtreeBucket<V1> {
        public:

            /**
             * Create a btree structure based on the json structure in @param 'spec', and set
             * @param 'id' to this btree.
             * @return the btree.
             *
             * For example the spec { b:{ a:null }, d:{ c:null }, _:{ e:null } } would create the
             * btree
             *
             *      [ b, d ]
             *     /   |    \
             * [ a ] [ c ] [ e ]
             *
             * Dummy record locations are populated based on the string values.  The first character
             * of each string value must be a hex digit.  See dummyRecordForKey().
             */
            static TestableBtree* set( const string& spec, IndexDetails& id ) {
                DiskLoc btree = make( spec, id );
                id.head = btree;
                return cast( btree );
            }

            /** Cast a disk location to a TestableBtree. */
            static TestableBtree* cast( const DiskLoc& l ) {
                return static_cast<TestableBtree*>( l.btreemod<V1>() );
            }

            /** Push a new key to this bucket. */
            void push( const BSONObj& key, DiskLoc child ) {
                KeyOwned k(key);
                pushBack( dummyRecordForKey( key ),
                          k,
                          Ordering::make( BSON( "a" << 1 ) ),
                          child );
            }

            /** Delete a key from this bucket. */
            void delKey( int index ) { _delKeyAtPos( index ); }

            /** Reset the number of keys for this bucket. */
            void setN( int newN ) { n = newN; }

            /** Set the right child for this bucket. */
            void setNext( const DiskLoc &child ) { nextChild = child; }

            /** A dummy record DiskLoc generated from a key's string value. */
            static DiskLoc dummyRecordForKey( const BSONObj& key ) {
                return DiskLoc( 0, fromHex( key.firstElement().String()[ 0 ] ) );
            }

        private:
            static DiskLoc make( const string& specString, IndexDetails& id ) {
                BSONObj spec = fromjson( specString );
                DiskLoc bucket = addBucket( id );
                cast( bucket )->init();
                TestableBtree* btree = TestableBtree::cast( bucket );
                BSONObjIterator i( spec );
                while( i.more() ) {
                    BSONElement e = i.next();
                    DiskLoc child;
                    if ( e.type() == Object ) {
                        child = make( e.embeddedObject().jsonString(), id );
                    }
                    if ( e.fieldName() == string( "_" ) ) {
                        btree->setNext( child );
                    }
                    else {
                        btree->push( BSON( "" << e.fieldName() ), child );
                    }
                }
                btree->fixParentPtrs( bucket );
                return bucket;
            }
        };

        /**
         * Helper to check that the expected key and its corresponding dummy record are located at
         * the supplied key location.
         */
        void assertKeyForPosition( const string& expectedKey,
                                   const BtreeKeyLocation& location ) {
            BucketBasics<V1>::KeyNode keyNode =
                    location.bucket.btree<V1>()->keyNode( location.pos );
            BSONObj expectedKeyObj = BSON( "" << expectedKey );
            ASSERT_EQUALS( expectedKeyObj, keyNode.key.toBson() );
            ASSERT_EQUALS( TestableBtree::dummyRecordForKey( expectedKeyObj ),
                           keyNode.recordLoc );
        }

        /** A btree position is recovered when the btree bucket is unmodified. */
        class RecoverPositionBucketUnchanged {
        public:
            void run() {
                Client::WriteContext ctx( _ns );
                _client.dropCollection( _ns );

                // Add an index and populate it with dummy keys.
                _client.ensureIndex( _ns, BSON( "a" << 1 ) );
                IndexDetails& idx = nsdetails( _ns )->idx( 1 );
                TestableBtree* btree = TestableBtree::set( "{b:{a:null},_:{c:null}}", idx );

                // Locate the 'a' key.
                BtreeKeyLocation aLocation( btree->keyNode( 0 ).prevChildBucket, 0 );

                // Try to recover the key location.
                Ordering ordering = Ordering::make( nsdetails( _ns )->idx( 1 ).keyPattern() );
                LogicalBtreePosition logical( idx, ordering, aLocation );
                logical.init();

                // Check that the original location is recovered.
                ASSERT_EQUALS( aLocation, logical.currentLocation() );

                // Invalidate the original location.
                logical.invalidateInitialLocation();

                // Check that the original location is still recovered.
                ASSERT_EQUALS( aLocation, logical.currentLocation() );
                assertKeyForPosition( "a", logical.currentLocation() );
            }
        };

        /** A btree position is recovered after its initial bucket is deallocated. */
        class RecoverPositionBucketDeallocated {
        public:
            void run() {
                Client::WriteContext ctx( _ns );
                _client.dropCollection( _ns );

                // Add an index and populate it with dummy keys.
                _client.ensureIndex( _ns, BSON( "a" << 1 ) );
                IndexDetails& idx = nsdetails( _ns )->idx( 1 );
                TestableBtree* btree = TestableBtree::set( "{b:{a:null},_:{c:null}}", idx );

                // Locate the 'c' key.
                BtreeKeyLocation cLocation( btree->getNextChild(), 0 );

                // Identify the key position.
                Ordering ordering = Ordering::make( nsdetails( _ns )->idx( 1 ).keyPattern() );
                LogicalBtreePosition logical( idx, ordering, cLocation );
                logical.init();

                // Invalidate the 'c' key's btree bucket.
                TestableBtree::cast( cLocation.bucket )->deallocBucket( cLocation.bucket, idx );

                // Add the 'c' key back to the tree, in the root bucket.
                btree->push( BSON( "" << "c" ),
                             TestableBtree::dummyRecordForKey( BSON( "" << "c" ) ) );

                // Check that the new location of 'c' is recovered.
                ASSERT_EQUALS( BtreeKeyLocation( idx.head, 1 ), logical.currentLocation() );
                assertKeyForPosition( "c", logical.currentLocation() );
            }
        };

        /** A btree position is recovered after its initial bucket shrinks. */
        class RecoverPositionKeyIndexInvalid {
        public:
            void run() {
                Client::WriteContext ctx( _ns );
                _client.dropCollection( _ns );

                // Add an index and populate it with dummy keys.
                _client.ensureIndex( _ns, BSON( "a" << 1 ) );
                IndexDetails& idx = nsdetails( _ns )->idx( 1 );
                TestableBtree::set( "{b:{a:null},c:null,_:{d:null}}", idx );

                // Locate the 'c' key.
                BtreeKeyLocation cLocation( idx.head, 1 );

                // Identify the key position.
                Ordering ordering = Ordering::make( nsdetails( _ns )->idx( 1 ).keyPattern() );
                LogicalBtreePosition logical( idx, ordering, cLocation );
                logical.init();

                // Remove the 'c' key by resizing the root bucket.
                TestableBtree::cast( cLocation.bucket )->setN( 1 );

                // Check that the location of 'd' is recovered.
                assertKeyForPosition( "d", logical.currentLocation() );
            }
        };

        /** A btree position is recovered after the key it refers to is removed. */
        class RecoverPositionKeyRemoved {
        public:
            void run() {
                Client::WriteContext ctx( _ns );
                _client.dropCollection( _ns );

                // Add an index and populate it with dummy keys.
                _client.ensureIndex( _ns, BSON( "a" << 1 ) );
                IndexDetails& idx = nsdetails( _ns )->idx( 1 );
                TestableBtree::set( "{b:{a:null},c:null,e:{d:null}}", idx );

                // Locate the 'c' key.
                BtreeKeyLocation cLocation( idx.head, 1 );

                // Identify the key position.
                Ordering ordering = Ordering::make( nsdetails( _ns )->idx( 1 ).keyPattern() );
                LogicalBtreePosition logical( idx, ordering, cLocation );
                logical.init();

                // Remove the 'c' key.
                TestableBtree::cast( cLocation.bucket )->delKey( 1 );

                // Check that the location of 'd' is recovered.
                assertKeyForPosition( "d", logical.currentLocation() );
            }
        };

        /**
         * A btree position is recovered after the key it refers to is removed, and a subsequent key
         * has the same record location.
         */
        class RecoverPositionKeyRemovedWithMatchingRecord {
        public:
            void run() {
                Client::WriteContext ctx( _ns );
                _client.dropCollection( _ns );

                // Add an index and populate it with dummy keys.
                _client.ensureIndex( _ns, BSON( "a" << 1 ) );
                IndexDetails& idx = nsdetails( _ns )->idx( 1 );
                TestableBtree* btree =
                        TestableBtree::set( "{b:{a:null},c:null,ccc:{cc:null}}", idx );

                // Verify that the 'c' key has the the same record location as the 'ccc' key, which
                // is a requirement of this test.
                ASSERT_EQUALS( btree->keyNode( 1 ).recordLoc, btree->keyNode( 2 ).recordLoc );

                // Locate the 'c' key.
                BtreeKeyLocation cLocation( idx.head, 1 );

                // Identify the key position.
                Ordering ordering = Ordering::make( nsdetails( _ns )->idx( 1 ).keyPattern() );
                LogicalBtreePosition logical( idx, ordering, cLocation );
                logical.init();

                // Remove the 'c' key.
                TestableBtree::cast( cLocation.bucket )->delKey( 1 );

                // Check that the location of 'cc' is recovered.
                assertKeyForPosition( "cc", logical.currentLocation() );
            }
        };

    } // namespace LogicalBtreePosition

    class All : public Suite {
    public:
        All() : Suite( "btreeposition" ) {
        }
        void setupTests() {
            add<BtreeKeyLocation::Equality>();
            add<LogicalBtreePosition::RecoverPositionBucketUnchanged>();
            add<LogicalBtreePosition::RecoverPositionBucketDeallocated>();
            add<LogicalBtreePosition::RecoverPositionKeyIndexInvalid>();
            add<LogicalBtreePosition::RecoverPositionKeyRemoved>();
            add<LogicalBtreePosition::RecoverPositionKeyRemovedWithMatchingRecord>();
        }
    } myall;

} // BtreePositionTests
