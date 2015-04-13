/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/range_arithmetic.h"
#include "mongo/dbtests/config_server_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/chunk.h" // for genID
#include "mongo/s/chunk_version.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/d_state.h"
#include "mongo/s/d_merge.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    using std::string;
    using std::vector;

    /**
     * Specialization of the config server fixture with helpers for the tests below.
     */
    class MergeChunkFixture: public ConfigServerFixture {
    public:

        MergeChunkFixture() : ConfigServerFixture() {

        }

        /**
         * Stores ranges for a particular collection and shard starting from some version
         */
        void storeCollectionRanges( const NamespaceString& nss,
                                    const string& shardName,
                                    const vector<KeyRange>& ranges,
                                    const ChunkVersion& startVersion ) {

            // Get key pattern from first range
            ASSERT_GREATER_THAN( ranges.size(), 0u );

            CollectionType coll;
            coll.setNs( nss.ns() );
            coll.setKeyPattern( ranges.begin()->keyPattern );
            coll.setEpoch( startVersion.epoch() );
            coll.setUpdatedAt( 1ULL );
            ASSERT_OK(coll.validate());

            DBDirectClient client(&_txn);

            client.update( CollectionType::ConfigNS,
                           BSON( CollectionType::fullNs( coll.getNs() ) ),
                           coll.toBSON(), true, false );

            ChunkVersion nextVersion = startVersion;
            for ( vector<KeyRange>::const_iterator it = ranges.begin(); it != ranges.end(); ++it ) {

                ChunkType chunk;
                // TODO: We should not rely on the serialized ns, minkey being unique in the future,
                // causes problems since it links string serialization to correctness.
                chunk.setName( Chunk::genID( nss, it->minKey ) );
                chunk.setShard( shardName );
                chunk.setNS( nss.ns() );
                chunk.setVersion( nextVersion );
                chunk.setMin( it->minKey );
                chunk.setMax( it->maxKey );
                nextVersion.incMajor();

                client.insert( ChunkType::ConfigNS, chunk.toBSON() );
            }
        }

        /**
         * Makes sure that all the ranges here no longer exist on disk but the merged range does
         */
        void assertWrittenAsMerged( const vector<KeyRange>& ranges ) {

            dumpServer();

            BSONObj rangeMin;
            BSONObj rangeMax;

            DBDirectClient client(&_txn);

            // Ensure written
            for( vector<KeyRange>::const_iterator it = ranges.begin(); it != ranges.end(); ++it ) {

                Query query( BSON( ChunkType::min( it->minKey ) <<
                                   ChunkType::max( it->maxKey ) <<
                                   ChunkType::shard( shardName() ) ) );
                ASSERT(client.findOne(ChunkType::ConfigNS, query).isEmpty());

                if ( rangeMin.isEmpty() || rangeMin.woCompare( it->minKey ) > 0 ) {
                    rangeMin = it->minKey;
                }

                if ( rangeMax.isEmpty() || rangeMax.woCompare( it->maxKey ) < 0 ) {
                    rangeMax = it->maxKey;
                }
            }

            Query query( BSON( ChunkType::min( rangeMin ) <<
                               ChunkType::max( rangeMax ) <<
                               ChunkType::shard( shardName() ) ) );
            ASSERT(!client.findOne(ChunkType::ConfigNS, query).isEmpty());
        }

    };

    //
    // Tests for upgrading the config server between versions.
    //
    // In general these tests do pretty minimal validation of the config server data itself, but
    // do ensure that the upgrade mechanism is working correctly w.r.t the config.version
    // collection.
    //

    // Rename the fixture so that our tests have a useful name in the executable
    typedef MergeChunkFixture MergeChunkTests;

    TEST_F(MergeChunkTests, FailedMerge) {

        const NamespaceString nss( "foo.bar" );
        const BSONObj kp = BSON( "x" << 1 );
        const OID epoch = OID::gen();
        vector<KeyRange> ranges;

        // Setup chunk metadata
        ranges.push_back( KeyRange( nss, BSON( "x" << 0 ), BSON( "x" << 10 ), kp ) );
        ranges.push_back( KeyRange( nss, BSON( "x" << 10 ), BSON( "x" << 20 ), kp ) );
        storeCollectionRanges( nss, shardName(), ranges, ChunkVersion( 1, 0, epoch ) );

        // Do bad merges
        string errMsg;
        bool result;

        result = mergeChunks(&_txn,  nss, BSON( "x" << 5 ), BSON( "x" << 20 ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );

        result = mergeChunks(&_txn,  nss, BSON( "x" << 0 ), BSON( "x" << 15 ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );

        result = mergeChunks(&_txn,  nss, BSON( "x" << -10 ), BSON( "x" << 20 ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );

        result = mergeChunks(&_txn,  nss, BSON( "x" << 0 ), BSON( "x" << 30 ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );

        result = mergeChunks(&_txn,  nss, BSON( "x" << 0 ), BSON( "x" << 10 ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );

        // Wrong epoch
        result = mergeChunks(&_txn,  nss, BSON( "x" << 0 ),
                                   BSON( "x" << 10 ), OID::gen(), &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );
    }

    TEST_F(MergeChunkTests, FailedMergeHole) {

        const NamespaceString nss( "foo.bar" );
        const BSONObj kp = BSON( "x" << 1 );
        const OID epoch = OID::gen();
        vector<KeyRange> ranges;

        // Setup chunk metadata
        ranges.push_back( KeyRange( nss, BSON( "x" << 0 ), BSON( "x" << 10 ), kp ) );
        ranges.push_back( KeyRange( nss, BSON( "x" << 11 ), BSON( "x" << 20 ), kp ) );
        storeCollectionRanges( nss, shardName(), ranges, ChunkVersion( 1, 0, epoch ) );

        // Do bad merge with hole
        string errMsg;
        bool result;
        result = mergeChunks(&_txn,  nss, BSON( "x" << 0 ), BSON( "x" << 20 ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );
    }

    TEST_F(MergeChunkTests, FailedMergeMinMax) {

        const NamespaceString nss( "foo.bar" );
        const BSONObj kp = BSON( "x" << 1 );
        const OID epoch = OID::gen();
        vector<KeyRange> ranges;

        // Setup chunk metadata
        ranges.push_back( KeyRange( nss, BSON( "x" << MINKEY ), BSON( "x" << 0 ), kp ) );
        ranges.push_back( KeyRange( nss, BSON( "x" << 0 ), BSON( "x" << MAXKEY ), kp ) );
        storeCollectionRanges( nss, shardName(), ranges, ChunkVersion( 1, 0, epoch ) );

        // Do bad merge with hole
        string errMsg;
        bool result;
        result = mergeChunks(&_txn,  nss, BSON( "x" << -1 ),
                                   BSON( "x" << MAXKEY ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );

        result = mergeChunks(&_txn,  nss, BSON( "x" << MINKEY ),
                                   BSON( "x" << 1 ), epoch, &errMsg );
        ASSERT_NOT_EQUALS( errMsg, "" );
        ASSERT( !result );
    }

    TEST_F(MergeChunkTests, BasicMerge) {

        const NamespaceString nss( "foo.bar" );
        const BSONObj kp = BSON( "x" << 1 );
        const OID epoch = OID::gen();
        vector<KeyRange> ranges;

        // Setup chunk metadata
        ranges.push_back( KeyRange( nss, BSON( "x" << 0 ), BSON( "x" << 1 ), kp ) );
        ranges.push_back( KeyRange( nss, BSON( "x" << 1 ), BSON( "x" << 2 ), kp ) );
        storeCollectionRanges( nss, shardName(), ranges, ChunkVersion( 1, 0, epoch ) );

        // Get latest version
        ChunkVersion latestVersion;
        shardingState.refreshMetadataNow(&_txn, nss, &latestVersion );
        shardingState.resetMetadata( nss );

        // Do merge
        string errMsg;
        bool result = mergeChunks(&_txn,  nss, BSON( "x" << 0 ), BSON( "x" << 2 ), epoch, &errMsg );
        ASSERT_EQUALS( errMsg, "" );
        ASSERT( result );

        // Verify result
        CollectionMetadataPtr metadata = shardingState.getCollectionMetadata( nss );

        ChunkType chunk;
        ASSERT( metadata->getNextChunk( BSON( "x" << 0 ), &chunk ) );
        ASSERT( chunk.getMin().woCompare( BSON( "x" << 0 ) ) == 0 );
        ASSERT( chunk.getMax().woCompare( BSON( "x" << 2 ) ) == 0 );
        ASSERT_EQUALS( metadata->getNumChunks(), 1u );

        ASSERT_EQUALS( metadata->getShardVersion().majorVersion(), latestVersion.majorVersion() );
        ASSERT_GREATER_THAN( metadata->getShardVersion().minorVersion(),
                             latestVersion.minorVersion() );

        assertWrittenAsMerged( ranges );
    }

    TEST_F(MergeChunkTests, BasicMergeMinMax ) {

        const NamespaceString nss( "foo.bar" );
        const BSONObj kp = BSON( "x" << 1 );
        const OID epoch = OID::gen();
        vector<KeyRange> ranges;

        // Setup chunk metadata
        ranges.push_back( KeyRange( nss, BSON( "x" << MINKEY ), BSON( "x" << 0 ), kp ) );
        ranges.push_back( KeyRange( nss, BSON( "x" << 0 ), BSON( "x" << MAXKEY ), kp ) );
        storeCollectionRanges( nss, shardName(), ranges, ChunkVersion( 1, 0, epoch ) );

        // Get latest version
        ChunkVersion latestVersion;
        shardingState.refreshMetadataNow(&_txn, nss, &latestVersion);
        shardingState.resetMetadata( nss );

        // Do merge
        string errMsg;
        bool result = mergeChunks(&_txn,  nss, BSON( "x" << MINKEY ),
                                        BSON( "x" << MAXKEY ), epoch, &errMsg );
        ASSERT_EQUALS( errMsg, "" );
        ASSERT( result );

        // Verify result
        CollectionMetadataPtr metadata = shardingState.getCollectionMetadata( nss );

        ChunkType chunk;
        ASSERT( metadata->getNextChunk( BSON( "x" << MINKEY ), &chunk ) );
        ASSERT( chunk.getMin().woCompare( BSON( "x" << MINKEY ) ) == 0 );
        ASSERT( chunk.getMax().woCompare( BSON( "x" << MAXKEY ) ) == 0 );
        ASSERT_EQUALS( metadata->getNumChunks(), 1u );

        ASSERT_EQUALS( metadata->getShardVersion().majorVersion(), latestVersion.majorVersion() );
        ASSERT_GREATER_THAN( metadata->getShardVersion().minorVersion(),
                             latestVersion.minorVersion() );

        assertWrittenAsMerged( ranges );
    }

    TEST_F(MergeChunkTests, CompoundMerge ) {

        const NamespaceString nss( "foo.bar" );
        const BSONObj kp = BSON( "x" << 1 << "y" << 1 );
        const OID epoch = OID::gen();
        vector<KeyRange> ranges;

        // Setup chunk metadata
        ranges.push_back( KeyRange( nss, BSON( "x" << 0 << "y" << 1 ),
                                         BSON( "x" << 1 << "y" << 0 ), kp ) );
        ranges.push_back( KeyRange( nss, BSON( "x" << 1 << "y" << 0 ),
                                         BSON( "x" << 2 << "y" << 1 ), kp ) );
        storeCollectionRanges( nss, shardName(), ranges, ChunkVersion( 1, 0, epoch ) );

        // Get latest version
        ChunkVersion latestVersion;
        shardingState.refreshMetadataNow(&_txn, nss, &latestVersion);
        shardingState.resetMetadata( nss );

        // Do merge
        string errMsg;
        bool result = mergeChunks(&_txn,  nss, BSON( "x" << 0 << "y" << 1 ),
                                        BSON( "x" << 2 << "y" << 1 ), epoch, &errMsg );
        ASSERT_EQUALS( errMsg, "" );
        ASSERT( result );

        // Verify result
        CollectionMetadataPtr metadata = shardingState.getCollectionMetadata( nss );

        ChunkType chunk;
        ASSERT( metadata->getNextChunk( BSON( "x" << 0 << "y" << 1 ), &chunk ) );
        ASSERT( chunk.getMin().woCompare( BSON( "x" << 0 << "y" << 1 ) ) == 0 );
        ASSERT( chunk.getMax().woCompare( BSON( "x" << 2 << "y" << 1 ) ) == 0 );
        ASSERT_EQUALS( metadata->getNumChunks(), 1u );

        ASSERT_EQUALS( metadata->getShardVersion().majorVersion(), latestVersion.majorVersion() );
        ASSERT_GREATER_THAN( metadata->getShardVersion().minorVersion(),
                             latestVersion.minorVersion() );

        assertWrittenAsMerged( ranges );
    }

} // end namespace
