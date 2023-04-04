/**
 * Ensure serverStatus reports the total time spent sampling the oplog for all storage engines that
 * support OplogTruncateMarkers.
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
            logComponentVerbosity: tojson({storage: {verbosity: 3}}),
        }
    }
});
replSet.startSet();
replSet.initiate();

let coll = replSet.getPrimary().getDB("test").getCollection("testcoll");

let res = replSet.getPrimary().getDB("test").serverStatus();
assert.commandWorked(res);

// Small (or empty) oplogs should be processed by scanning.
assert.gte(res.oplogTruncation.totalTimeProcessingMicros, 0);
assert.eq(res.oplogTruncation.processingMethod, "scanning");

// Insert enough documents to force oplog sampling to occur on the following start up.
// Ensure that fast count of oplog collection increases while we insert the documents.
const oplogColl = replSet.getPrimary().getDB("local").getCollection("oplog.rs");
let oplogFastCount = oplogColl.count();
const maxOplogDocsForScanning = 2000;
jsTestLog("Inserting " + maxOplogDocsForScanning + " documents to force oplog sampling on restart");
for (let i = 0; i < maxOplogDocsForScanning + 1; i++) {
    let doc = {m: 1 + i};
    assert.commandWorked(coll.insert(doc), "failed to insert " + tojson(doc));

    let newOplogFastCount = oplogColl.count();
    assert.gt(
        newOplogFastCount,
        oplogFastCount,
        "fast count of oplog collection did not increase after successfully inserting " +
            tojson(doc) + ". Previous fast count of oplog: " + oplogFastCount +
            ". New fast count: " + newOplogFastCount + ". Last 5 oplog entries: " +
            tojson(replSet.findOplog(replSet.getPrimary(), /*query=*/ {}, /*limit=*/ 5).toArray()));
    oplogFastCount = newOplogFastCount;
}

// Do not proceed with test if the oplog collection has a lower than expected fast count that
// will result in an oplog scan on restart.
assert.gt(
    oplogFastCount,
    maxOplogDocsForScanning,
    "fast count of oplog collection is not large enough to trigger oplog sampling on restart");

// Restart replica set to load entries from the oplog for sampling.
jsTestLog("Inserted " + maxOplogDocsForScanning + " documents. Oplog fast count: " +
          oplogFastCount + ". Restarting server to force oplog sampling.");
replSet.stopSet(null /* signal */, true /* forRestart */);
replSet.startSet({restart: true});

res = replSet.getPrimary().getDB("test").serverStatus();
assert.commandWorked(res);

assert.gt(res.oplogTruncation.totalTimeProcessingMicros, 0);
assert.eq(res.oplogTruncation.processingMethod, "sampling");

replSet.stopSet();
})();
