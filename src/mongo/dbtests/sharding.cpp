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

#include "mongo/pch.h"

#include "mongo/client/dbclientmockcursor.h"
#include "mongo/client/parallel.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/type_chunk.h"

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

    static int rand( int max = -1 ){
        static unsigned seed = 1337;

#if !defined(_WIN32)
        int r = rand_r( &seed ) ;
#else
        int r = ::rand(); // seed not used in this case
#endif

        // Modding is bad, but don't really care in this case
        return max > 0 ? r % max : r;
    }

    //
    // Sets up a basic environment for loading chunks to/from the direct database connection
    // Redirects connections to the direct database for the duration of the test.
    //
    class ChunkManagerTest : public ConnectionString::ConnectionHook {
    public:

        class CustomDirectClient : public DBDirectClient {
        public:
            virtual ConnectionString::ConnectionType type() const {
                return ConnectionString::CUSTOM;
            }
        };

        CustomDirectClient _client;
        Shard _shard;

        ChunkManagerTest() {

            DBException::traceExceptions = true;

            // Make all connections redirect to the direct client
            ConnectionString::setConnectionHook( this );

            // Create the default config database before querying, necessary for direct connections
            client().dropDatabase( "config" );
            client().insert( "config.test", BSON( "hello" << "world" ) );
            client().dropCollection( "config.test" );

            client().dropDatabase( nsGetDB( collName() ) );
            client().insert( collName(), BSON( "hello" << "world" ) );
            client().dropCollection( collName() );

            // Since we've redirected the conns, the host doesn't matter here so long as it's
            // prefixed with a "$"
            _shard = Shard( "shard0000", "$hostFooBar:27017" );
            // Need to run this to ensure the shard is in the global lookup table
            _shard.setAddress( _shard.getAddress() );

            // Create an index so that diffing works correctly, otherwise no cursors from S&O
            client().ensureIndex( ChunkType::ConfigNS, // br
                                  BSON( ChunkType::ns() << 1 << // br
                                          ChunkType::DEPRECATED_lastmod() << 1 ) );
        }

        virtual ~ChunkManagerTest() {
            // Reset the redirection
            ConnectionString::setConnectionHook( NULL );
        }

        string collName(){ return "foo.bar"; }

        Shard& shard(){ return _shard; }

        DBDirectClient& client(){
            return _client;
        }

        virtual DBClientBase* connect( const ConnectionString& connStr,
                                       string& errmsg,
                                       double socketTimeout )
        {
            // Note - must be new, since it gets owned elsewhere
            return new CustomDirectClient();
        }
    };

    //
    // Tests creating a new chunk manager and creating the default chunks
    //
    class ChunkManagerCreateBasicTest : public ChunkManagerTest {
    public:

        void run(){

            ChunkManager manager( collName(), ShardKeyPattern( BSON( "_id" << 1 ) ), false );
            manager.createFirstChunks( shard().getConnString(), shard(), NULL, NULL );

            BSONObj firstChunk = client().findOne(ChunkType::ConfigNS, BSONObj()).getOwned();

            ASSERT(firstChunk[ChunkType::min()].Obj()[ "_id" ].type() == MinKey );
            ASSERT(firstChunk[ChunkType::max()].Obj()[ "_id" ].type() == MaxKey );

            ChunkVersion version = ChunkVersion::fromBSON(firstChunk,
                                                          ChunkType::DEPRECATED_lastmod());

            ASSERT( version.majorVersion() == 1 );
            ASSERT( version.minorVersion() == 0 );
            ASSERT( version.isEpochSet() );

        }

    };

    //
    // Tests creating a new chunk manager with random split points.  Creating chunks on multiple shards is not
    // tested here since there are unresolved race conditions there and probably should be avoided if at all
    // possible.
    //
    class ChunkManagerCreateFullTest : public ChunkManagerTest {
    public:

        static const int numSplitPoints = 100;

        void genRandomSplitPoints( vector<int>* splitPoints ){
            for( int i = 0; i < numSplitPoints; i++ ){
                splitPoints->push_back( rand( numSplitPoints * 10 ) );
            }
        }

        void genRandomSplitKeys( const string& keyName, vector<BSONObj>* splitKeys ){
            vector<int> splitPoints;
            genRandomSplitPoints( &splitPoints );

            for( vector<int>::iterator it = splitPoints.begin(); it != splitPoints.end(); ++it ){
                splitKeys->push_back( BSON( keyName << *it ) );
            }
        }

        // Uses a chunk manager to create chunks
        void createChunks( const string& keyName ){

            vector<BSONObj> splitKeys;
            genRandomSplitKeys( keyName, &splitKeys );

            ChunkManager manager( collName(), ShardKeyPattern( BSON( keyName << 1 ) ), false );

            manager.createFirstChunks( shard().getConnString(), shard(), &splitKeys, NULL );
        }

        void run(){

            string keyName = "_id";
            createChunks( keyName );

            auto_ptr<DBClientCursor> cursor =
                client().query(ChunkType::ConfigNS, QUERY(ChunkType::ns(collName())));

            set<int> minorVersions;
            OID epoch;

            // Check that all chunks were created with version 1|x with consistent epoch and unique minor versions
            while( cursor->more() ){

                BSONObj chunk = cursor->next();

                ChunkVersion version = ChunkVersion::fromBSON(chunk,
                                                              ChunkType::DEPRECATED_lastmod());

                ASSERT( version.majorVersion() == 1 );
                ASSERT( version.isEpochSet() );

                if( ! epoch.isSet() ) epoch = version.epoch();
                ASSERT( version.epoch() == epoch );

                ASSERT( minorVersions.find( version.minorVersion() ) == minorVersions.end() );
                minorVersions.insert( version.minorVersion() );

                ASSERT(chunk[ChunkType::shard()].String() == shard().getName());
            }
        }

    };

    //
    // Tests that chunks are loaded correctly from the db with no a-priori info and also that they can be reloaded
    // on top of an old chunk manager with changes.
    //
    class ChunkManagerLoadBasicTest : public ChunkManagerCreateFullTest {
    public:

        void run(){

            string keyName = "_id";
            createChunks( keyName );
            int numChunks = static_cast<int>(client().count(ChunkType::ConfigNS,
                                                            BSON(ChunkType::ns(collName()))));

            BSONObj firstChunk = client().findOne(ChunkType::ConfigNS, BSONObj()).getOwned();

            ChunkVersion version = ChunkVersion::fromBSON(firstChunk,
                                                          ChunkType::DEPRECATED_lastmod());

            // Make manager load existing chunks
            ChunkManagerPtr manager( new ChunkManager( collName(), ShardKeyPattern( BSON( "_id" << 1 ) ), false ) );
            ((ChunkManager*) manager.get())->loadExistingRanges( shard().getConnString() );

            ASSERT( manager->getVersion().epoch() == version.epoch() );
            ASSERT( manager->getVersion().minorVersion() == ( numChunks - 1 ) );
            ASSERT( static_cast<int>( manager->getChunkMap().size() ) == numChunks );

            // Modify chunks collection
            BSONObjBuilder b;
            ChunkVersion laterVersion = ChunkVersion( 2, 1, version.epoch() );
            laterVersion.addToBSON(b, ChunkType::DEPRECATED_lastmod());

            client().update(ChunkType::ConfigNS, BSONObj(), BSON( "$set" << b.obj()));

            // Make new manager load chunk diff
            ChunkManager newManager( manager );
            newManager.loadExistingRanges( shard().getConnString() );

            ASSERT( newManager.getVersion().toLong() == laterVersion.toLong() );
            ASSERT( newManager.getVersion().epoch() == laterVersion.epoch() );
            ASSERT( static_cast<int>( newManager.getChunkMap().size() ) == numChunks );
        }

    };

    class ChunkDiffUnitTest {
    public:

        bool _inverse;

        typedef map<BSONObj, BSONObj, BSONObjCmp> RangeMap;
        typedef map<string, ChunkVersion> VersionMap;

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

        // Allow validating with and without ranges (b/c our splits won't actually be updated by the diffs)
        void validate( BSONArray chunks, ChunkVersion maxVersion, const VersionMap& maxShardVersions ){
            validate( chunks, NULL, maxVersion, maxShardVersions );
        }

        void validate( BSONArray chunks, const RangeMap& ranges, ChunkVersion maxVersion, const VersionMap& maxShardVersions ){
            validate( chunks, (RangeMap*)&ranges, maxVersion, maxShardVersions );
        }

        // Validates that the ranges and versions are valid given the chunks
        void validate( const BSONArray& chunks, RangeMap* ranges, ChunkVersion maxVersion, const VersionMap& maxShardVersions ){

            BSONObjIterator it( chunks );
            int chunkCount = 0;
            ChunkVersion foundMaxVersion;
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

                ChunkVersion version =
                    ChunkVersion::fromBSON(chunkDoc[ChunkType::DEPRECATED_lastmod()]);
                if( version > foundMaxVersion ) foundMaxVersion = version;

                ChunkVersion shardMaxVersion =
                    foundMaxShardVersions[chunkDoc[ChunkType::shard()].String()];
                if( version > shardMaxVersion ) {
                    foundMaxShardVersions[chunkDoc[ChunkType::shard()].String() ] = version;
                }
            }

            // Make sure all chunks are accounted for
            if( ranges != NULL ) ASSERT( chunkCount == (int) ranges->size() );

            // log() << "Validating that all shard versions are up to date..." << endl;

            // Validate that all the versions are the same
            ASSERT( foundMaxVersion.isEquivalentTo( maxVersion ) );

            for( VersionMap::iterator it = foundMaxShardVersions.begin(); it != foundMaxShardVersions.end(); it++ ){

                ChunkVersion foundVersion = it->second;
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
            ChunkVersion version( 1, 0, OID() );

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

                    chunkB.append(ChunkType::min(), lastSplitPt );
                    chunkB.append(ChunkType::max(), splitPt );

                    int shardNum = rand( numShards );
                    chunkB.append(ChunkType::shard(),
                                  "shard" + string( 1, (char)('A' + shardNum) ) );

                    rand( 2 ) ? version.incMajor() : version.incMinor();
                    version.addToBSON(chunkB, ChunkType::DEPRECATED_lastmod());

                    chunksB.append( chunkB.obj() );
                }

                lastSplitPt = splitPt;
            }

            BSONArray chunks = chunksB.arr();

            // log() << "Chunks generated : " << chunks << endl;

            DBClientMockCursor chunksCursor( chunks );

            // Setup the empty ranges and versions first
            RangeMap ranges;
            ChunkVersion maxVersion = ChunkVersion( 0, 0, OID() );
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

                            BSONType maxType = chunk[ChunkType::max()].Obj()[field].type();
                            double max = maxType == NumberDouble ? chunk["max"].Obj()[field].Number() : 0.0;
                            BSONType minType = chunk[ChunkType::min()].Obj()[field].type();
                            double min = minType == NumberDouble ?
                                                    chunk[ChunkType::min()].Obj()[field].Number() :
                                                    0.0;

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

                        leftB.append( chunk[ChunkType::min()] );
                        leftB.append(ChunkType::max(), midPt );
                        rightB.append(ChunkType::min(), midPt );
                        rightB.append(chunk[ChunkType::max()] );

                        leftB.append(chunk[ChunkType::shard()] );
                        rightB.append(chunk[ChunkType::shard()] );

                        version.incMajor();
                        version._minor = 0;
                        version.addToBSON(leftB, ChunkType::DEPRECATED_lastmod());
                        version.incMinor();
                        version.addToBSON(rightB, ChunkType::DEPRECATED_lastmod());

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
                            if( prevShardChunk[ChunkType::shard()].String() ==
                                chunk[ChunkType::shard()].String() ) break;

                            // log() << "... appending chunk from diff shard: " << prevShardChunk << endl;
                            newChunksB.append( prevShardChunk );

                            prevShardChunk = BSONObj();
                        }

                        // We need to move between different shards, hence the weirdness in logic here
                        if( ! prevShardChunk.isEmpty() ){

                            BSONObjBuilder newShardB;
                            BSONObjBuilder prevShardB;

                            newShardB.append(chunk[ChunkType::min()]);
                            newShardB.append(chunk[ChunkType::max()]);
                            prevShardB.append(prevShardChunk[ChunkType::min()]);
                            prevShardB.append(prevShardChunk[ChunkType::max()]);

                            int shardNum = rand( numShards );
                            newShardB.append(ChunkType::shard(),
                                             "shard" + string( 1, (char)('A' + shardNum)));
                            prevShardB.append(prevShardChunk[ChunkType::shard()]);

                            version.incMajor();
                            version._minor = 0;
                            version.addToBSON(newShardB, ChunkType::DEPRECATED_lastmod());
                            version.incMinor();
                            version.addToBSON(prevShardB, ChunkType::DEPRECATED_lastmod());

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
                    maxVersion = ChunkVersion( 0, 0, OID() );
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
            add< ChunkManagerCreateBasicTest >();
            add< ChunkManagerCreateFullTest >();
            add< ChunkManagerLoadBasicTest >();
            add< ChunkDiffUnitTestNormal >();
            add< ChunkDiffUnitTestInverse >();
        }
    } myall;

}
