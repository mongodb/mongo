// Tests that replaying the oplog entries during the startup recovery also writes to the change
// collection.
// @tags: [
//   featureFlagServerlessChangeStreams,
//   multiversion_incompatible,
//   featureFlagMongoStore,
// ]

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");                    // For configureFailPoint.
load("jstests/serverless/libs/change_collection_util.js");  // For verifyChangeCollectionEntries.

const replSetTest = new ReplSetTest({nodes: 1});

// TODO SERVER-67267 add 'featureFlagServerlessChangeStreams', 'multitenancySupport' and
// 'serverless' flags and remove 'failpoint.forceEnableChangeCollectionsMode'.
replSetTest.startSet(
    {setParameter: "failpoint.forceEnableChangeCollectionsMode=" + tojson({mode: "alwaysOn"})});

replSetTest.initiate();

let primary = replSetTest.getPrimary();

// Insert a document to the collection and then capture the corresponding oplog timestamp. This
// timestamp will be the start timestamp beyond (inclusive) which we will validate the oplog and the
// change collection entries.
const startTimestamp = assert
                           .commandWorked(primary.getDB("test").runCommand(
                               {insert: "seedCollection", documents: [{_id: "beginTs"}]}))
                           .operationTime;

// Pause the checkpointing, as such non-journaled collection including the change collection will
// not be persisted.
const pauseCheckpointThreadFailPoint = configureFailPoint(primary, "pauseCheckpointThread");
pauseCheckpointThreadFailPoint.wait();

// Insert a document to the collection.
assert.commandWorked(primary.getDB("test").stockPrice.insert({_id: "mdb", price: 250}));

// Verify that the inserted document can be queried from the 'stockPrice', the 'oplog.rs', and
// the 'system.change_collection'.
assert.eq(primary.getDB("test").stockPrice.find({_id: "mdb", price: 250}).toArray().length, 1);
assert.eq(primary.getDB("local")
              .oplog.rs.find({ns: "test.stockPrice", o: {_id: "mdb", price: 250}})
              .toArray()
              .length,
          1);
assert.eq(primary.getDB("config")
              .system.change_collection.find({ns: "test.stockPrice", o: {_id: "mdb", price: 250}})
              .toArray()
              .length,
          1);

// Perform ungraceful shutdown of the primary node and do not clean the db path directory.
replSetTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

// Run a new mongoD instance with db path pointing to the replica set primary db directory.
const standalone =
    MongoRunner.runMongod({dbpath: primary.dbpath, noReplSet: true, noCleanData: true});
assert.neq(null, standalone, "Fail to restart the node as standalone");

// Verify that the inserted document does not exist both in the 'stockPrice' and
// the 'system.change_collection' but exists in the 'oplog.rs'.
assert.eq(standalone.getDB("test").stockPrice.find({_id: "mdb", price: 250}).toArray().length, 0);
assert.eq(standalone.getDB("local")
              .oplog.rs.find({ns: "test.stockPrice", o: {_id: "mdb", price: 250}})
              .toArray()
              .length,
          1);
assert.eq(standalone.getDB("config")
              .system.change_collection.find({ns: "test.stockPrice", o: {_id: "mdb", price: 250}})
              .toArray()
              .length,
          0);

// Stop the mongoD instance and do not clean the db directory.
MongoRunner.stopMongod(standalone, null, {noCleanData: true, skipValidation: true, wait: true});

// Start the replica set primary with the same db path.
replSetTest.start(primary, {
    noCleanData: true,
    setParameter: "failpoint.forceEnableChangeCollectionsMode=" + tojson({mode: "alwaysOn"})
});

primary = replSetTest.getPrimary();

// Verify that the 'stockPrice' and the 'system.change_collection' now have the inserted document.
// This document was inserted by applying oplog entries during the startup recovery.
assert.eq(primary.getDB("test").stockPrice.find({_id: "mdb", price: 250}).toArray().length, 1);
assert.eq(primary.getDB("config")
              .system.change_collection.find({ns: "test.stockPrice", o: {_id: "mdb", price: 250}})
              .toArray()
              .length,
          1);

// Get the oplog timestamp up to this point. All oplog entries upto this timestamp must exist in the
// change collection.
const endTimestamp = primary.getDB("local").oplog.rs.find().toArray().at(-1).ts;
assert(endTimestamp !== undefined);

// Verify that the oplog and the change collection entries between the ['startTimestamp',
// 'endTimestamp'] window are exactly same and in the same order.
verifyChangeCollectionEntries(primary, startTimestamp, endTimestamp);

replSetTest.stopSet();
})();
