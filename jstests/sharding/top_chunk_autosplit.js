function shardSetup(shardConfig, dbName, collName) {
    var st = new ShardingTest(shardConfig);
    var db = st.getDB(dbName);

    // Set the balancer mode to only balance on autoSplit
    assert.writeOK(st.s.getDB('config').settings.update(
        {_id: 'balancer'}, {'$unset': {stopped: ''}, '$set': {mode: 'autoSplitOnly'}}));
    return st;
}

function getShardWithTopChunk(configDB, lowOrHigh) {
    // lowOrHigh: 1 low "top chunk", -1 high "top chunk"
    return configDB.chunks.find({}).sort({min: lowOrHigh}).limit(1).next().shard;
}

function getNumberOfChunks(configDB) {
    return configDB.chunks.count();
}

function runTest(test) {
    jsTest.log('Running: ' + tojson(test));

    // Setup
    // Shard collection
    assert.commandWorked(db.adminCommand({shardCollection: coll + "", key: {x: 1}}));

    // Pre-split, move chunks & create tags
    for (var i = 0; i < test.shards.length; i++) {
        var startRange = test.shards[i].range.min;
        var endRange = test.shards[i].range.max;
        var chunkSize = Math.abs(endRange - startRange) / test.shards[i].chunks;
        for (var j = startRange; j < endRange; j += chunkSize) {
            // No split on highest chunk
            if (j + chunkSize >= MAXVAL) {
                continue;
            }
            assert.commandWorked(db.adminCommand({split: coll + "", middle: {x: j + chunkSize}}));
            db.adminCommand({moveChunk: coll + "", find: {x: j}, to: test.shards[i].name});
        }

        // Make sure to move chunk when there's only 1 chunk in shard
        db.adminCommand({moveChunk: coll + "", find: {x: startRange}, to: test.shards[i].name});

        // Make sure to move highest chunk
        if (test.shards[i].range.max == MAXVAL) {
            db.adminCommand({moveChunk: coll + "", find: {x: MAXVAL}, to: test.shards[i].name});
        }

        // Add tags to each shard
        var tags = test.shards[i].tags || [];
        for (j = 0; j < tags.length; j++) {
            sh.addShardTag(test.shards[i].name, tags[j]);
        }
    }

    // Add tag ranges associated to a tag
    var tagRanges = test.tagRanges || [];
    for (var j = 0; j < tagRanges.length; j++) {
        sh.addTagRange(db + "." + collName,
                       {x: tagRanges[j].range.min},
                       {x: tagRanges[j].range.max},
                       tagRanges[j].tag);
    }

    // Number of chunks before auto-split
    var numChunks = getNumberOfChunks(configDB);
    // End of setup

    // Insert on top chunk to force auto-split
    var largeStr = new Array(1000).join('x');

    // Insert one doc at a time until first auto-split occurs on top chunk
    var xval = test.inserts.value;
    do {
        var doc = {x: xval, val: largeStr};
        coll.insert(doc);
        xval += test.inserts.inc;
    } while (getNumberOfChunks(configDB) <= numChunks);

    // Test for where new top chunk should reside
    assert.eq(getShardWithTopChunk(configDB, test.lowOrHigh),
              test.movedToShard,
              test.name + " chunk in the wrong shard");

    // Cleanup: Drop collection, tags & tag ranges
    coll.drop();
    for (var i = 0; i < test.shards.length; i++) {
        var tags = test.shards[i].tags || [];
        for (j = 0; j < tags.length; j++) {
            sh.removeShardTag(test.shards[i].name, tags[j]);
        }
    }

    assert.writeOK(configDB.tags.remove({ns: db + "." + collName}));
    // End of test cleanup
}

// Define shard key ranges for each of the shard nodes
var MINVAL = -500;
var MAXVAL = 1500;
var lowChunkRange = {min: MINVAL, max: 0};
var midChunkRange1 = {min: 0, max: 500};
var midChunkRange2 = {min: 500, max: 1000};
var highChunkRange = {min: 1000, max: MAXVAL};

var lowChunkTagRange = {min: MinKey, max: 0};
var highChunkTagRange = {min: 1000, max: MaxKey};

var lowChunkInserts = {value: 0, inc: -1};
var midChunkInserts = {value: 1, inc: 1};
var highChunkInserts = {value: 1000, inc: 1};

var lowChunk = 1;
var highChunk = -1;

// Test objects:
//   name - name of test
//   lowOrHigh - 1 for low top chunk, -1 for high top chunk
//   movedToShard - name of shard the new top chunk should reside on
//   shards - array of shard objects
//            name - name of shard
//            range - range object of shard key
//            chunks - number of chunks to pre-split on this shard
//            tags - array of tags associated to this shard
//   tagRanges - array of objects defining the tag range
//            range - range object of shard key
//            tag - tag associated to this range
//   inserts - object for inserting on shard key from low to high
//            low - low shard key value
//            high - high shard key value
var tests = [
    {
      // Test auto-split on the "low" top chunk to another tagged shard
      name: "low top chunk with tag move",
      lowOrHigh: lowChunk,
      movedToShard: "shard0002",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 20, tags: ["NYC"]},
          {name: "shard0001", range: midChunkRange1, chunks: 20, tags: ["SF"]},
          {name: "shard0002", range: highChunkRange, chunks: 5, tags: ["NYC"]},
          {name: "shard0003", range: midChunkRange2, chunks: 1, tags: ["SF"]},
      ],
      tagRanges: [
          {range: lowChunkTagRange, tag: "NYC"},
          {range: highChunkTagRange, tag: "NYC"},
          {range: midChunkRange1, tag: "SF"},
          {range: midChunkRange2, tag: "SF"}
      ],
      inserts: lowChunkInserts
    },
    {
      // Test auto-split on the "low" top chunk to same tagged shard
      name: "low top chunk with tag no move",
      lowOrHigh: lowChunk,
      movedToShard: "shard0000",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 5, tags: ["NYC"]},
          {name: "shard0001", range: midChunkRange1, chunks: 20, tags: ["SF"]},
          {name: "shard0002", range: highChunkRange, chunks: 20, tags: ["NYC"]},
          {name: "shard0003", range: midChunkRange2, chunks: 1, tags: ["SF"]},
      ],
      tagRanges: [
          {range: lowChunkTagRange, tag: "NYC"},
          {range: highChunkTagRange, tag: "NYC"},
          {range: midChunkRange1, tag: "SF"},
          {range: midChunkRange2, tag: "SF"}
      ],
      inserts: lowChunkInserts
    },
    {
      // Test auto-split on the "low" top chunk to another shard
      name: "low top chunk no tag move",
      lowOrHigh: lowChunk,
      movedToShard: "shard0003",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 20},
          {name: "shard0001", range: midChunkRange1, chunks: 20},
          {name: "shard0002", range: highChunkRange, chunks: 5},
          {name: "shard0003", range: midChunkRange2, chunks: 1}
      ],
      inserts: lowChunkInserts
    },
    {
      // Test auto-split on the "high" top chunk to another tagged shard
      name: "high top chunk with tag move",
      lowOrHigh: highChunk,
      movedToShard: "shard0000",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 5, tags: ["NYC"]},
          {name: "shard0001", range: midChunkRange1, chunks: 20, tags: ["SF"]},
          {name: "shard0002", range: highChunkRange, chunks: 20, tags: ["NYC"]},
          {name: "shard0003", range: midChunkRange2, chunks: 1, tags: ["SF"]}
      ],
      tagRanges: [
          {range: lowChunkTagRange, tag: "NYC"},
          {range: highChunkTagRange, tag: "NYC"},
          {range: midChunkRange1, tag: "SF"},
          {range: midChunkRange2, tag: "SF"}
      ],
      inserts: highChunkInserts
    },
    {
      // Test auto-split on the "high" top chunk to another shard
      name: "high top chunk no tag move",
      lowOrHigh: highChunk,
      movedToShard: "shard0003",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 5},
          {name: "shard0001", range: midChunkRange1, chunks: 20},
          {name: "shard0002", range: highChunkRange, chunks: 20},
          {name: "shard0003", range: midChunkRange2, chunks: 1}
      ],
      inserts: highChunkInserts
    },
    {
      // Test auto-split on the "high" top chunk to same tagged shard
      name: "high top chunk with tag no move",
      lowOrHigh: highChunk,
      movedToShard: "shard0002",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 20, tags: ["NYC"]},
          {name: "shard0001", range: midChunkRange1, chunks: 20, tags: ["SF"]},
          {name: "shard0002", range: highChunkRange, chunks: 5, tags: ["NYC"]},
          {name: "shard0003", range: midChunkRange2, chunks: 1, tags: ["SF"]}
      ],
      tagRanges: [
          {range: lowChunkTagRange, tag: "NYC"},
          {range: highChunkTagRange, tag: "NYC"},
          {range: midChunkRange1, tag: "SF"},
          {range: midChunkRange2, tag: "SF"}
      ],
      inserts: highChunkInserts
    },
    {
      // Test auto-split on the "high" top chunk to same shard
      name: "high top chunk no tag no move",
      lowOrHigh: highChunk,
      movedToShard: "shard0002",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 20},
          {name: "shard0001", range: midChunkRange1, chunks: 20},
          {name: "shard0002", range: highChunkRange, chunks: 1},
          {name: "shard0003", range: midChunkRange2, chunks: 5}
      ],
      inserts: highChunkInserts
    },
];

var singleNodeTests = [
    {
      // Test auto-split on the "low" top chunk on single node shard
      name: "single node shard - low top chunk",
      lowOrHigh: lowChunk,
      movedToShard: "shard0000",
      shards: [{name: "shard0000", range: lowChunkRange, chunks: 2}],
      inserts: lowChunkInserts
    },
    {
      // Test auto-split on the "high" top chunk on single node shard
      name: "single node shard - high top chunk",
      lowOrHigh: highChunk,
      movedToShard: "shard0000",
      shards: [{name: "shard0000", range: highChunkRange, chunks: 2}],
      inserts: highChunkInserts
    },
];

var maxSizeTests = [
    {
      // Test auto-split on the "low" top chunk with maxSize on destination shard
      name: "maxSize - low top chunk",
      lowOrHigh: lowChunk,
      movedToShard: "shard0000",
      shards: [
          {name: "shard0000", range: lowChunkRange, chunks: 10},
          {name: "shard0001", range: highChunkRange, chunks: 1}
      ],
      inserts: lowChunkInserts
    },
    {
      // Test auto-split on the "high" top chunk with maxSize on destination shard
      name: "maxSize - high top chunk",
      lowOrHigh: highChunk,
      movedToShard: "shard0000",
      shards: [
          {name: "shard0000", range: highChunkRange, chunks: 10},
          {name: "shard0001", range: lowChunkRange, chunks: 1}
      ],
      inserts: highChunkInserts
    },
];

// Main
var dbName = "TopChunkDB";
var collName = "coll";

var st = shardSetup({name: "topchunk", shards: 4, chunkSize: 1}, dbName, collName);
var db = st.getDB(dbName);
var coll = db[collName];
var configDB = st.s.getDB('config');

assert.commandWorked(db.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, 'shard0000');

// Execute all test objects
for (var i = 0; i < tests.length; i++) {
    runTest(tests[i]);
}

st.stop();

// Single node shard tests
st = shardSetup({name: "singleNode", shards: 1, chunkSize: 1}, dbName, collName);
db = st.getDB(dbName);
coll = db[collName];
configDB = st.s.getDB('config');

assert.commandWorked(db.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, 'shard0000');

// Execute all test objects
for (var i = 0; i < singleNodeTests.length; i++) {
    runTest(singleNodeTests[i]);
}

st.stop();

// maxSize test
// To set maxSize, must manually add the shards
st = shardSetup(
    {name: "maxSize", shards: 2, chunkSize: 1, other: {manualAddShard: true}}, dbName, collName);
db = st.getDB(dbName);
coll = db[collName];
configDB = st.s.getDB('config');

// maxSize on shard0000 - 5MB, on shard0001 - 1MB
assert.commandWorked(db.adminCommand({addshard: st.getConnNames()[0], maxSize: 5}));
assert.commandWorked(db.adminCommand({addshard: st.getConnNames()[1], maxSize: 1}));

// SERVER-17070 Auto split moves to shard node running WiredTiger, if exceeding maxSize
var unsupported = ["wiredTiger", "rocksdb", "inMemory", "ephemeralForTest"];
if (unsupported.indexOf(st.d0.adminCommand({serverStatus: 1}).storageEngine.name) == -1 &&
    unsupported.indexOf(st.d1.adminCommand({serverStatus: 1}).storageEngine.name) == -1) {
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, 'shard0000');

    // Execute all test objects
    for (var i = 0; i < maxSizeTests.length; i++) {
        runTest(maxSizeTests[i]);
    }
}

st.stop();
