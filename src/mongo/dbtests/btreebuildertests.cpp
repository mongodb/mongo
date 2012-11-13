//@file btreebuildertests.cpp : mongo/db/btreebuilder.{h,cpp} tests

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

#include "mongo/db/btreebuilder.h"

#include "mongo/db/btreecursor.h"
#include "mongo/db/pdfile.h"
#include "mongo/platform/cstdint.h"

#include "mongo/dbtests/dbtests.h"

namespace BtreeBuilderTests {

    static const char* const _ns = "unittests.btreebuilder";
    DBDirectClient _client;

    /**
     * Test fixture for a write locked test using collection _ns.  Includes functionality to
     * partially construct a new IndexDetails in a manner that supports proper cleanup in
     * dropCollection().
     */
    class IndexBuildBase {
    public:
        IndexBuildBase() :
            _ctx( _ns ) {
            _client.createCollection( _ns );
        }
        ~IndexBuildBase() {
            _client.dropCollection( _ns );
        }
    protected:
        /** @return IndexDetails for a new index on a:1, with the info field populated. */
        IndexDetails& addIndexWithInfo() {
            BSONObj indexInfo = BSON( "v" << 1 <<
                                      "key" << BSON( "a" << 1 ) <<
                                      "ns" << _ns <<
                                      "name" << "a_1" );
            int32_t lenWHdr = indexInfo.objsize() + Record::HeaderSize;
            const char* systemIndexes = "unittests.system.indexes";
            DiskLoc infoLoc = allocateSpaceForANewRecord( systemIndexes,
                                                          nsdetails( systemIndexes ),
                                                          lenWHdr,
                                                          false );
            Record* infoRecord = reinterpret_cast<Record*>( getDur().writingPtr( infoLoc.rec(),
                                                                                 lenWHdr ) );
            memcpy( infoRecord->data(), indexInfo.objdata(), indexInfo.objsize() );
            addRecordToRecListInExtent( infoRecord, infoLoc );
            IndexDetails& id = nsdetails( _ns )->getNextIndexDetails( _ns );
            nsdetails( _ns )->addIndex( _ns );
            id.info.writing() = infoLoc;
            return id;
        }
    private:
        Client::WriteContext _ctx;
    };

    /**
     * BtreeBuilder::commit() constructs a btree from the keys provided to BtreeBuilder::addKey().
     */
    class Commit : public IndexBuildBase {
    public:
        void run() {
            IndexDetails& id = addIndexWithInfo();
            // Create a btree builder.
            BtreeBuilder<V1> builder( false, id );
            // Add some keys to the builder, in order.
            int32_t nKeys = 1000;
            for( int32_t i = 0; i < nKeys; ++i ) {
                BSONObj key = BSON( "a" << i );
                builder.addKey( key, /* dummy location */ DiskLoc() );
            }
            // The root of the index has not yet been set.
            ASSERT( id.head.isNull() );
            // Call commit on the builder to finish building the btree.
            builder.commit( true );
            // The root of the index is now set.
            ASSERT( !id.head.isNull() );
            // Create a cursor over the index.
            scoped_ptr<BtreeCursor> cursor(
                    BtreeCursor::make( nsdetails( _ns ),
                                       id,
                                       BSON( "" << -1 ),    // startKey below minimum key value.
                                       BSON( "" << nKeys ), // endKey above maximum key value.
                                       true,                // endKeyInclusive true.
                                       1                    // direction forward.
                                       ) );
            // Check that the keys in the index are the expected ones.
            int32_t expectedKey = 0;
            for( ; cursor->ok(); cursor->advance(), ++expectedKey ) {
                ASSERT_EQUALS( expectedKey, cursor->currKey().firstElement().number() );
            }
            ASSERT_EQUALS( nKeys, expectedKey );
        }
    };

    /**
     * BtreeBuilder::commit() is interrupted if there is a request to kill the current operation.
     */
    class InterruptCommit : public IndexBuildBase {
    public:
        InterruptCommit( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            IndexDetails& id = addIndexWithInfo();
            // Create a btree builder.
            BtreeBuilder<V1> builder( false, id );
            // Add some keys to the builder, in order.  We need enough keys to build an internal
            // node in order to check for an interrupt.
            int32_t nKeys = 1000;
            for( int32_t i = 0; i < nKeys; ++i ) {
                BSONObj key = BSON( "a" << i );
                builder.addKey( key, /* dummy location */ DiskLoc() );
            }
            // The root of the index has not yet been set.
            ASSERT( id.head.isNull() );
            // Register a request to kill the current operation.
            cc().curop()->kill();
            if ( _mayInterrupt ) {
                // Call commit on the builder, which will be aborted due to the kill request.
                ASSERT_THROWS( builder.commit( _mayInterrupt ), UserException );
                // The root of the index is not set because commit() did not complete.
                ASSERT( id.head.isNull() );
            }
            else {
                // Call commit on the builder, which will not be aborted because mayInterrupt is
                // false.
                builder.commit( _mayInterrupt );
                // The root of the index is set because commit() completed.
                ASSERT( !id.head.isNull() );
            }
        }
    private:
        bool _mayInterrupt;
    };

    class BtreeBuilderTests : public Suite {
    public:
        BtreeBuilderTests() :
            Suite( "btreebuilder" ) {
        }

        void setupTests() {
            add<Commit>();
            add<InterruptCommit>( false );
            add<InterruptCommit>( true );
        }
    } btreeBuilderTests;

} // namespace BtreeBuilderTests
