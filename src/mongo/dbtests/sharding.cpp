// sharding.cpp : some unit tests for sharding internals

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "pch.h"

#include "dbtests.h"

#include "../client/parallel.h"
#include "../client/dbclientmockcursor.h"
#include "../s/chunk_diff.h"

namespace ShardingTests {

    namespace serverandquerytests {
        class test1 {
        public:
            void run() {
                ServerAndQuery a( "foo:1" , BSON( "a" << GT << 0 << LTE << 100 ) );
                ServerAndQuery b( "foo:1" , BSON( "a" << GT << 200 << LTE << 1000 ) );

                ASSERT( a < b );
                ASSERT( ! ( b < a ) );

                set<ServerAndQuery> s;
                s.insert( a );
                s.insert( b );

                ASSERT_EQUALS( (unsigned int)2 , s.size() );
            }
        };
    }

    class ChunkManagerTest : public ConnectionString::ConnectionHook {
    public:

        static DBDirectClient _client;

        ChunkManagerTest() {
            // Make all connections redirect to the direct client
            ConnectionString::setConnectionHook( this );

            // Create the default config database before querying, necessary for direct connections
            client().dropDatabase( "config" );
            client().insert( "config.test", BSON( "hello" << "world" ) );
            client().dropCollection( "config.test" );

            client().dropDatabase( nsGetDB( collName() ) );
            client().insert( collName(), BSON( "hello" << "world" ) );
            client().dropCollection( collName() );
        }

        string collName(){ return "foo.bar"; }

        virtual ~ChunkManagerTest() {
            // Reset the redirection
            ConnectionString::setConnectionHook( NULL );
        }

        DBDirectClient& client(){
            return _client;
        }

        virtual DBClientBase* connect( const ConnectionString& connStr,
                                         string& errmsg,
                                         double socketTimeout )
        {
            // Note - must be new, since it gets owned elsewhere
            return new DBDirectClient();
        }
    };

    DBDirectClient ChunkManagerTest::_client;

    class ChunkManagerCreateBasicTest : public ChunkManagerTest {
    public:

        void run(){

            BSONObj key = BSON( "_id" << 1 );
            bool unique = false;

            // Since we've redirected the conns, the host doesn't matter here
            Shard shard( "shard0000", "hostFooBar:27017" );

            ChunkManager manager( collName(), ShardKeyPattern( key ), unique );
            manager.createFirstChunks( shard.getConnString(), shard, NULL, NULL );

            BSONObj firstChunk = client().findOne( ShardNS::chunk, BSONObj() );

            ASSERT( firstChunk[ "min" ].Obj()[ "_id" ].type() == MinKey );
            ASSERT( firstChunk[ "max" ].Obj()[ "_id" ].type() == MaxKey );

            ShardChunkVersion version = ShardChunkVersion::fromBSON( firstChunk, "lastmod" );

            ASSERT( version.majorVersion() == 1 );
            ASSERT( version.minorVersion() == 0 );
            ASSERT( version.isEpochSet() );

        }

    };

    class ChunkDiffUnitTest {
    public:

        bool _inverse;

        typedef map<BSONObj, BSONObj, BSONObjCmp> RangeMap;
        typedef map<string, ShardChunkVersion> VersionMap;

        ChunkDiffUnitTest( bool inverse ) : _inverse( inverse ) {}

        // The default pass-through adapter for using config diffs
        class DefaultDiffAdapter : public ConfigDiffTracker<BSONObj,string> {
        public:

            DefaultDiffAdapter() {}
            virtual ~DefaultDiffAdapter() {}

            virtual bool isTracked( const BSONObj& chunkDoc ) const { return true; }
            virtual BSONObj maxFrom( const BSONObj& max ) const { return max; }

            virtual pair<BSONObj,BSONObj> rangeFor( const BSONObj& chunkDoc, const BSONObj& min, const BSONObj& max ) const {
                return make_pair( min, max );
            }

            virtual string shardFor( const string& name ) const { return name; }
            virtual string nameFrom( const string& shard ) const { return shard; }
        };

        // Inverts the storage order for chunks from min to max
        class InverseDiffAdapter : public DefaultDiffAdapter {
        public:

            InverseDiffAdapter() {}
            virtual ~InverseDiffAdapter() {}

            // Disable
            virtual BSONObj maxFrom( const BSONObj& max ) const { ASSERT( false ); return max; }
            virtual BSONObj minFrom( const BSONObj& min ) const { return min; }

            virtual bool isMinKeyIndexed() const { return false; }

            virtual pair<BSONObj,BSONObj> rangeFor( const BSONObj& chunkDoc, const BSONObj& min, const BSONObj& max ) const {
                return make_pair( max, min );
            }
        };

        int rand( int max = -1 ){
            static unsigned seed = 1337;

#if !defined(_WIN32)
            int r = rand_r( &seed ) ;
#else
            int r = ::rand(); // seed not used in this case
#endif

            // Modding is bad, but don't really care in this case
            return max > 0 ? r % max : r;
        }

        // Allow validating with and without ranges (b/c our splits won't actually be updated by the diffs)
        void validate( BSONArray chunks, ShardChunkVersion maxVersion, const VersionMap& maxShardVersions ){
            validate( chunks, NULL, maxVersion, maxShardVersions );
        }

        void validate( BSONArray chunks, const RangeMap& ranges, ShardChunkVersion maxVersion, const VersionMap& maxShardVersions ){
            validate( chunks, (RangeMap*)&ranges, maxVersion, maxShardVersions );
        }

        // Validates that the ranges and versions are valid given the chunks
        void validate( const BSONArray& chunks, RangeMap* ranges, ShardChunkVersion maxVersion, const VersionMap& maxShardVersions ){

            BSONObjIterator it( chunks );
            int chunkCount = 0;
            ShardChunkVersion foundMaxVersion;
            VersionMap foundMaxShardVersions;

            //
            // Validate that all the chunks are there and collect versions
            //

            while( it.more() ){

                BSONObj chunkDoc = it.next().Obj();
                chunkCount++;

                if( ranges != NULL ){

                    // log() << "Validating chunk " << chunkDoc << " size : " << ranges->size() << " vs " << chunkCount << endl;

                    RangeMap::iterator chunkRange = ranges->find( _inverse ? chunkDoc["max"].Obj() : chunkDoc["min"].Obj() );

                    ASSERT( chunkRange != ranges->end() );
                    ASSERT( chunkRange->second.woCompare( _inverse ? chunkDoc["min"].Obj() : chunkDoc["max"].Obj() ) == 0 );
                }

                ShardChunkVersion version = ShardChunkVersion::fromBSON( chunkDoc["lastmod"] );
                if( version > foundMaxVersion ) foundMaxVersion = version;

                ShardChunkVersion shardMaxVersion = foundMaxShardVersions[ chunkDoc["shard"].String() ];
                if( version > shardMaxVersion ) foundMaxShardVersions[ chunkDoc["shard"].String() ] = version;
            }
            // Make sure all chunks are accounted for
            if( ranges != NULL ) ASSERT( chunkCount == (int) ranges->size() );

            // log() << "Validating that all shard versions are up to date..." << endl;

            // Validate that all the versions are the same
            ASSERT( foundMaxVersion.isEquivalentTo( maxVersion ) );

            for( VersionMap::iterator it = foundMaxShardVersions.begin(); it != foundMaxShardVersions.end(); it++ ){

                ShardChunkVersion foundVersion = it->second;
                VersionMap::const_iterator maxIt = maxShardVersions.find( it->first );

                ASSERT( maxIt != maxShardVersions.end() );
                ASSERT( foundVersion.isEquivalentTo( maxIt->second ) );
            }
            // Make sure all shards are accounted for
            ASSERT( foundMaxShardVersions.size() == maxShardVersions.size() );
        }

        void run() {

            int numShards = 10;
            int numInitialChunks = 5;
            int maxChunks = 100000; // Needed to not overflow the BSONArray's max bytes
            int keySize = 2;

            BSONArrayBuilder chunksB;

            BSONObj lastSplitPt;
            ShardChunkVersion version( 1, 0, OID() );

            //
            // Generate numChunks with a given key size over numShards
            // All chunks have double key values, so we can split them a bunch
            //

            for( int i = -1; i < numInitialChunks; i++ ){

                BSONObjBuilder splitPtB;
                for( int k = 0; k < keySize; k++ ){
                    string field = string( "k" ) + string( 1, (char)('0' + k) );
                    if( i < 0 )
                        splitPtB.appendMinKey( field );
                    else if( i < numInitialChunks - 1 )
                        splitPtB.append( field, (double)i );
                    else
                        splitPtB.appendMaxKey( field );
                }
                BSONObj splitPt = splitPtB.obj();

                if( i >= 0 ){
                    BSONObjBuilder chunkB;

                    chunkB.append( "min", lastSplitPt );
                    chunkB.append( "max", splitPt );

                    int shardNum = rand( numShards );
                    chunkB.append( "shard", "shard" + string( 1, (char)('A' + shardNum) ) );

                    rand( 2 ) ? version.incMajor() : version.incMinor();
                    version.addToBSON( chunkB, "lastmod" );

                    chunksB.append( chunkB.obj() );
                }

                lastSplitPt = splitPt;
            }

            BSONArray chunks = chunksB.arr();

            // log() << "Chunks generated : " << chunks << endl;

            DBClientMockCursor chunksCursor( chunks );

            // Setup the empty ranges and versions first
            RangeMap ranges;
            ShardChunkVersion maxVersion = ShardChunkVersion( 0, 0, OID() );
            VersionMap maxShardVersions;

            // Create a differ which will track our progress
            boost::shared_ptr< DefaultDiffAdapter > differ( _inverse ? new InverseDiffAdapter() : new DefaultDiffAdapter() );
            differ->attach( "test", ranges, maxVersion, maxShardVersions );

            // Validate initial load
            differ->calculateConfigDiff( chunksCursor );
            validate( chunks, ranges, maxVersion, maxShardVersions );

            // Generate a lot of diffs, and keep validating that updating from the diffs always
            // gives us the right ranges and versions

            int numDiffs = 135; // Makes about 100000 chunks overall
            int numChunks = numInitialChunks;
            for( int i = 0; i < numDiffs; i++ ){

                // log() << "Generating new diff... " << i << endl;

                BSONArrayBuilder diffsB;
                BSONArrayBuilder newChunksB;
                BSONObjIterator chunksIt( chunks );

                while( chunksIt.more() ){

                    BSONObj chunk = chunksIt.next().Obj();

                    int randChoice = rand( 10 );

                    if( randChoice < 2 && numChunks < maxChunks ){
                        // Simulate a split

                        // log() << " ...starting a split with chunk " << chunk << endl;

                        BSONObjBuilder leftB;
                        BSONObjBuilder rightB;
                        BSONObjBuilder midB;

                        for( int k = 0; k < keySize; k++ ){
                            string field = string( "k" ) + string( 1, (char)('0' + k) );

                            BSONType maxType = chunk["max"].Obj()[field].type();
                            double max = maxType == NumberDouble ? chunk["max"].Obj()[field].Number() : 0.0;
                            BSONType minType = chunk["min"].Obj()[field].type();
                            double min = minType == NumberDouble ? chunk["min"].Obj()[field].Number() : 0.0;

                            if( minType == MinKey ){
                                midB.append( field, max - 1.0 );
                            }
                            else if( maxType == MaxKey ){
                                midB.append( field, min + 1.0 );
                            }
                            else {
                                midB.append( field, ( max + min ) / 2.0 );
                            }
                        }

                        BSONObj midPt = midB.obj();
                        // Only happens if we can't split the min chunk
                        if( midPt.isEmpty() ) continue;

                        leftB.append( chunk["min"] );
                        leftB.append( "max", midPt );
                        rightB.append( "min", midPt );
                        rightB.append( chunk["max"] );

                        leftB.append( chunk["shard"] );
                        rightB.append( chunk["shard"] );

                        version.incMajor();
                        version._minor = 0;
                        version.addToBSON( leftB, "lastmod" );
                        version.incMinor();
                        version.addToBSON( rightB, "lastmod" );

                        BSONObj left = leftB.obj();
                        BSONObj right = rightB.obj();

                        // log() << " ... split into " << left << " and " << right << endl;

                        newChunksB.append( left );
                        newChunksB.append( right );

                        diffsB.append( right );
                        diffsB.append( left );

                        numChunks++;
                    }
                    else if( randChoice < 4 && chunksIt.more() ){
                        // Simulate a migrate

                        // log() << " ...starting a migrate with chunk " << chunk << endl;

                        BSONObj prevShardChunk;
                        while( chunksIt.more() ){
                            prevShardChunk = chunksIt.next().Obj();
                            if( prevShardChunk["shard"].String() == chunk["shard"].String() ) break;

                            // log() << "... appending chunk from diff shard: " << prevShardChunk << endl;
                            newChunksB.append( prevShardChunk );

                            prevShardChunk = BSONObj();
                        }

                        // We need to move between different shards, hence the weirdness in logic here
                        if( ! prevShardChunk.isEmpty() ){

                            BSONObjBuilder newShardB;
                            BSONObjBuilder prevShardB;

                            newShardB.append( chunk["min"] );
                            newShardB.append( chunk["max"] );
                            prevShardB.append( prevShardChunk["min"] );
                            prevShardB.append( prevShardChunk["max"] );

                            int shardNum = rand( numShards );
                            newShardB.append( "shard", "shard" + string( 1, (char)('A' + shardNum) ) );
                            prevShardB.append( prevShardChunk["shard"] );

                            version.incMajor();
                            version._minor = 0;
                            version.addToBSON( newShardB, "lastmod" );
                            version.incMinor();
                            version.addToBSON( prevShardB, "lastmod" );

                            BSONObj newShard = newShardB.obj();
                            BSONObj prevShard = prevShardB.obj();

                            // log() << " ... migrated to " << newShard << " and updated " << prevShard << endl;

                            newChunksB.append( newShard );
                            newChunksB.append( prevShard );

                            diffsB.append( newShard );
                            diffsB.append( prevShard );

                        }
                        else{
                            // log() << "... appending chunk, no more left: " << chunk << endl;
                            newChunksB.append( chunk );
                        }
                    }
                    else{
                        // log() << "Appending chunk : " << chunk << endl;
                        newChunksB.append( chunk );
                    }

                }

                BSONArray diffs = diffsB.arr();
                chunks = newChunksB.arr();

                // log() << "Diffs generated : " << diffs << endl;
                // log() << "All chunks : " << chunks << endl;

                // Rarely entirely clear out our data
                if( rand( 10 ) < 1 ){
                    diffs = chunks;
                    ranges.clear();
                    maxVersion = ShardChunkVersion( 0, 0, OID() );
                    maxShardVersions.clear();
                }

                // log() << "Total number of chunks : " << numChunks << " iteration " << i << endl;

                DBClientMockCursor diffCursor( diffs );

                differ->calculateConfigDiff( diffCursor );

                validate( chunks, ranges, maxVersion, maxShardVersions );

            }

        }
    };

    class ChunkDiffUnitTestNormal : public ChunkDiffUnitTest {
    public:
        ChunkDiffUnitTestNormal() : ChunkDiffUnitTest( false ) {}
    };

    class ChunkDiffUnitTestInverse : public ChunkDiffUnitTest {
    public:
        ChunkDiffUnitTestInverse() : ChunkDiffUnitTest( true ) {}
    };

    class All : public Suite {
    public:
        All() : Suite( "sharding" ) {
        }

        void setupTests() {
            add< serverandquerytests::test1 >();
            // SERVER-5918
            //add< ChunkManagerCreateBasicTest >();
            add< ChunkDiffUnitTestNormal >();
            add< ChunkDiffUnitTestInverse >();
        }
    } myall;

}
