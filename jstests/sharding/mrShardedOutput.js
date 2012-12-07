// This test writes documents to a 'foo' collection in two passes, and runs a 'null' MapReduce job
// with sharded output on the collection after each pass.  The map stage of the MapReduce job
// simply emits all of its input documents (only the 1 KiB field) and the reduce stage returns its
// input document.  Checks are made that all inserted records make it into the 'foo' collection,
// and that the sharded output collection is evenly distributed across the shards.

jsTest.log("Setting up new ShardingTest");
var st = new ShardingTest( "mrShardedOutput", 2, 1, 1, { chunksize : 1 } );

var config = st.getDB("config");
st.adminCommand( { enablesharding: "test" } );
st.adminCommand( { shardcollection: "test.foo", key: { "a": 1 } } );

var testDB = st.getDB( "test" );
var aaa = "aaaaaaaaaaaaaaaa";
var str = aaa;
while (str.length < 1*1024) { str += aaa; }

st.printChunks();
st.printChangeLog();

function map2() { emit(this._id, {count: 1, y: this.y}); }
function reduce2(key, values) { return values[0]; }

var numDocs = 0;
var buildIs32bits = (testDB.serverBuildInfo().bits == 32);
var numBatch = buildIs32bits ? (30 * 1000) : (100 * 1000);
var numChunks = 0;

var numIterations = 2;
for (var iter = 0; iter < numIterations; ++iter) {
    jsTest.log("Iteration " + iter + ": saving new batch of " + numBatch + " documents");

    // Add some more data for input so that chunks will get split further
    for (var i = 0; i < numBatch; ++i) {
        if (i % 1000 == 0) {
            print("\n========> Saved total of " + (numDocs + i) + " documents\n");
        }
        testDB.foo.save( {a: Math.random() * 1000, y: str, i: numDocs + i} );
    }
    print("\n========> Finished saving total of " + (numDocs + i) + " documents");

    var GLE = testDB.getLastError();
    assert.eq(null, GLE, "Setup FAILURE: testDB.getLastError() returned" + GLE);
    jsTest.log("No errors on insert batch.");
    numDocs += numBatch;

    var savedCount = testDB.foo.find().itcount();
    if (savedCount != numDocs) {
        jsTest.log("Setup FAILURE: testDB.foo.find().itcount() = " + savedCount +
                   " after saving " + numDocs + " documents -- (will assert after diagnostics)");

        // Stop balancer
        jsTest.log("Stopping balancer");
        st.stopBalancer();

        // Wait for writebacks
        jsTest.log("Waiting 10 seconds for writebacks");
        sleep(10000);

        jsTest.log("Checking for missing documents");
        for (i = 0; i < numDocs; ++i) {
            if ( !testDB.foo.findOne({ i: i }, { i: 1 }) ) {
                print("\n========> Could not find document " + i + "\n");
            }
            if (i % 1000 == 0) {
                print( "\n========> Checked " + i + "\n");
            }
        }
        print("\n========> Finished checking " + i + "\n");
        printShardingStatus(config, true);

        // Verify that WriteBackListener weirdness isn't causing this
        jsTest.log("Waiting for count to become correct");
        assert.soon(function() { var c = testDB.foo.find().itcount();
                                 print( "Count is " + c );
                                 return c == numDocs; },
                    "Setup FAILURE: Count did not become correct after 30 seconds",
                    /* timeout */  30 * 1000,
                    /* interval */ 1000);

        assert(false,
               "Setup FAILURE: getLastError was null for insert, but inserted count was wrong");
    }


    // Do the MapReduce step
    jsTest.log("Setup OK: count matches (" + numDocs + ") -- Starting MapReduce");
    var res = testDB.foo.mapReduce(map2, reduce2, {out: {replace: "mrShardedOut", sharded: true}});
    var reduceOutputCount = res.counts.output;
    assert.eq(numDocs,
              reduceOutputCount,
              "MapReduce FAILED: res.counts.output = " + reduceOutputCount +
                   ", should be " + numDocs);
    jsTest.log("MapReduce results:");
    printjson(res);

    jsTest.log("Checking that all MapReduce output documents are in output collection");
    var outColl = testDB["mrShardedOut"];
    var outCollCount = outColl.find().itcount();    // SERVER-3645 -can't use count()
    assert.eq(numDocs,
              outCollCount,
              "MapReduce FAILED: outColl.find().itcount() = " + outCollCount +
                   ", should be " + numDocs +
                   ": this may happen intermittently until resolution of SERVER-3627");

    // Make sure it's sharded and split
    var newNumChunks = config.chunks.count({ns: testDB.mrShardedOut._fullName});
    print("Number of chunks: " + newNumChunks);
    assert.gt(newNumChunks,
              1,
              "Sharding FAILURE: " + testDB.mrShardedOut._fullName + " has only 1 chunk");

    // Make sure num of chunks increases over time
    if (numChunks) {
        assert.gt(newNumChunks,
                  numChunks,
                  "Sharding FAILURE: Number of chunks did not increase between iterations");
    }
    numChunks = newNumChunks;

    // Check that chunks are well distributed
    printShardingStatus(config, true);
    jsTest.log("Checking chunk distribution");
    cur = config.chunks.find({ns: testDB.mrShardedOut._fullName});
    shardChunks = {};
    while (cur.hasNext()) {
        var chunk = cur.next();
        var shardName = chunk.shard;
        if (shardChunks[shardName]) {
            shardChunks[shardName] += 1;
        }
        else {
            shardChunks[shardName] = 1;
        }
    }
    var count = 0;
    for (var prop in shardChunks) {
        print("Number of chunks for shard " + prop + ": " + shardChunks[prop]);
        if (count == 0) {
            count = shardChunks[prop];
        }
        assert.lt(Math.abs(count - shardChunks[prop]),
                  numChunks / 10,
                  "Chunks are not well balanced: " + count + " vs. " + shardChunks[prop]);
    }
}

jsTest.log("SUCCESS!  Stopping ShardingTest");
st.stop();
