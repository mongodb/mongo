/**
 * Ensure serverStatus reports the total time spent sampling the oplog for all storage engines that
 * support OplogStones.
 * @tags: [ requires_persistence ]
 */
(function() {
"use strict";

// Force oplog sampling to occur on start up for small numbers of oplog inserts.
const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            "maxOplogTruncationPointsDuringStartup": 10,
            logComponentVerbosity: tojson({storage: {verbosity: 2}}),
        }
    }
});
replSet.startSet();
replSet.initiate();

let coll = replSet.getPrimary().getDB("test").getCollection("testcoll");

let res = replSet.getPrimary().getDB("test").serverStatus();
assert.commandWorked(res);

// Small (or empty) oplogs should be processed by scanning.
assert.gt(res.oplogTruncation.totalTimeProcessingMicros, 0);
assert.eq(res.oplogTruncation.processingMethod, "scanning");

// Insert enough documents to force oplog sampling to occur on the following start up.
const maxOplogDocsForScanning = 2000;
for (let i = 0; i < maxOplogDocsForScanning + 1; i++) {
    assert.commandWorked(coll.insert({m: 1 + i}));
}

// Ensure we have enough oplog entries in the oplog to avoid scanning.
// Check counts reported by aggregation and fast count.
const numOplogEntries = replSet.getPrimary().getDB("local").oplog.rs.countDocuments({});
jsTestLog('Number of oplog entries before restarting (aggregation): ' + numOplogEntries);
assert.gte(numOplogEntries, maxOplogDocsForScanning);
const numOplogEntriesFastCount = replSet.getPrimary().getDB("local").oplog.rs.count();
jsTestLog('Number of oplog entries before restarting (fast count): ' + numOplogEntriesFastCount);
assert.gte(numOplogEntriesFastCount, maxOplogDocsForScanning);
assert.lte(numOplogEntriesFastCount, numOplogEntries);

// Restart replica set to load entries from the oplog for sampling.
replSet.stopSet(null /* signal */, true /* forRestart */);
replSet.startSet({restart: true});

res = replSet.getPrimary().getDB("test").serverStatus();
assert.commandWorked(res);

assert.gt(res.oplogTruncation.totalTimeProcessingMicros, 0);
assert.eq(res.oplogTruncation.processingMethod, "sampling");

replSet.stopSet();
})();
