/**
 * SERVER-4987 This test tries to check the getLastError call will still use
 * the same connection even if a split chunk triggered while doing inserts
 * failed (cause by StaleConfigException).
 * 
 * TODO: SERVER-5175
 * This test relies on the corresponding delays inside (1) WriteBackListener::run
 * and (2) ShardStrategy::_insert and (3) receivedInsert from instance.cpp
 * to make the bug easier to manifest.
 * 
 * The purpose of (1) is to make the writebacks slower so the failed inserts won't
 * be reapplied on time.
 * 
 * The purpose of (2) is to make it easier for the moveChunk command from the other
 * mongos to interleave in between the moment the insert has set its shard version and
 * when in tries to autosplit (Note: it should be long enough to allow the moveChunk
 * to actually complete before it tries to proceed to autosplit).
 * 
 * The purpose of (3) is to make sure that the insert won't get applied to the
 * shard right away so when a different connection is used to do the getLastError,
 * the write will still not be applied.
 */
function testGleAfterSplitDuringMigration(){
    var st = new ShardingTest({ shards: 2, verbose: 2, mongos: 2,
                                other: { chunksize: 1 }});

    // Stop the balancer to prevent it from contending with the distributed lock.
    st.stopBalancer();

    var DB_NAME = jsTest.name();
    var COLL_NAME = "coll";

    var mongos = st.s0;
    var confDB = mongos.getDB( "config" );
    var coll = mongos.getCollection( DB_NAME + "." + COLL_NAME );

    var shardConn = st.d0;
    var shardColl = shardConn.getCollection( coll.getFullName() );

    var data = "x";
    var dataSize = 1024 * 256; // bytes, must be power of 2
    while( data.length < dataSize ) data += data;

    // Shard collection
    st.shardColl( coll, { _id : 1 }, false );

    var docID = 0;

    /**
     * @return {Mongo} the connection object given the name of the shard.
     */
    var getShardConn = function( shardName ) {
        var shardLoc = confDB.shards.findOne({ _id: shardName }).host;
        return new Mongo( shardLoc );
    };

    /**
     * Inserts documents using a direct shard connection to the max key chunk
     * enough to make sure that it will trigger the auto split.
     * 
     * variables from outer scope: docID, coll, confDB, data
     */
    var primeForSplitting = function() {
        var topChunk = confDB.chunks.find().sort({ max: -1 }).limit( 1 ).next();
        var shardLoc = getShardConn( topChunk.shard );
        var testColl = shardLoc.getCollection( coll.getFullName() );

        var superSaturatedChunkSize = 1024 * 1024 * 10; // 10MB
        var docsToSaturateChunkSize = superSaturatedChunkSize / dataSize;
        
        for ( var i = 0; i < docsToSaturateChunkSize; i++ ) {
            testColl.insert({ _id: docID++, val: data });
        }

        assert.eq( null, testColl.getDB().getLastError() );
    };

    /**
     * Moves a random chunk to a new shard using a different mongos.
     * 
     * @param tries {Number} number of retry attempts when the moveChunk command
     *    fails.
     * 
     * variables from outer scope: coll, st
     */
    var moveRandomChunk = function( tries ) {
        var otherConfDB = st.s1.getDB( "config" );
        var chunksCursor = otherConfDB.chunks.find().sort({ max: 1 });
        var chunkCount = chunksCursor.count();

        var randIdx = Math.floor( Math.random() * chunkCount );
        // Don't get the chunk with max/min key
        randIdx = ( randIdx == chunkCount )? randIdx - 1 : randIdx;
        randIdx = ( randIdx == 0 )? randIdx + 1 : randIdx;

        var chunk = chunksCursor.arrayAccess( randIdx );
        var chunkOwner = chunk.shard;
        var newOwner = otherConfDB.shards.findOne({ _id: { $ne: chunkOwner }})._id;

        var result = otherConfDB.adminCommand({ moveChunk: coll.getFullName(),
                                                find: { _id: chunk.min._id },
                                                to: newOwner });

        jsTest.log( "moveChunk result: " + tojson( result ));
        if( !result.ok && tries > 1 ) {
            moveRandomChunk( tries - 1 );
        }
    };

    var chunks = 0;
    do {
        coll.insert({ _id: docID++, val: data });
        chunks = mongos.getDB( "config" ).chunks.find().count();
    } while ( chunks < 5 );

    primeForSplitting();
    
    jsTest.log( "Starting the insert that should trigger auto-split." );

    // TODO: SERVER-5175 Trigger delays here
    coll.insert({ _id: docID++, val: data });
    moveRandomChunk( 3 );

    // getLastError should wait for all writes to this connection.
    var errObj = coll.getDB().getLastErrorObj();
    jsTest.log( "Last Error Object: " + tojson( errObj ));

    assert.eq( docID, coll.find().itcount(), "Count does not match!" );

    jsTest.log( "Finished counting." );

    st.stop();
}

testGleAfterSplitDuringMigration();

