// Tests that replaying the oplog entries during the startup recovery also writes to the change
// collection.
// @tags: [
//   requires_fcv_62,
// ]

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");                    // For configureFailPoint.
load("jstests/serverless/libs/change_collection_util.js");  // For verifyChangeCollectionEntries.

const replSetTest =
    new ReplSetTest({nodes: 1, name: "ChangeStreamMultitenantReplicaSetTest", serverless: true});

replSetTest.startSet({
    setParameter: {
        featureFlagServerlessChangeStreams: true,
        multitenancySupport: true,
        featureFlagMongoStore: true,
        featureFlagRequireTenantID: true
    }
});
replSetTest.initiate();

let primary = replSetTest.getPrimary();
const tenantId = ObjectId();

// Enable the change stream to create the change collection.
assert.commandWorked(
    primary.getDB("admin").runCommand({setChangeStreamState: 1, enabled: true, $tenant: tenantId}));

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
assert.commandWorked(primary.getDB("test").runCommand(
    {insert: "stockPrice", documents: [{_id: "mdb", price: 250}], $tenant: tenantId}));

// Verify that the inserted document can be queried from the 'stockPrice', the 'oplog.rs', and
// the 'system.change_collection'.
assert.eq(assert
              .commandWorked(primary.getDB("test").runCommand(
                  {find: "stockPrice", filter: {_id: "mdb", price: 250}, $tenant: tenantId}))
              .cursor.firstBatch.length,
          1);
assert.eq(assert
              .commandWorked(primary.getDB("local").runCommand(
                  {find: "oplog.rs", filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}}}))
              .cursor.firstBatch.length,
          1);
assert.eq(assert
              .commandWorked(primary.getDB("config").runCommand({
                  find: "system.change_collection",
                  filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}},
                  $tenant: tenantId
              }))
              .cursor.firstBatch.length,
          1);

// Perform ungraceful shutdown of the primary node and do not clean the db path directory.
replSetTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

// Run a new mongoD instance with db path pointing to the replica set primary db directory.
const standalone = MongoRunner.runMongod({
    setParameter: {
        featureFlagServerlessChangeStreams: true,
        multitenancySupport: true,
        featureFlagMongoStore: true,
        featureFlagRequireTenantID: true
    },
    dbpath: primary.dbpath,
    noReplSet: true,
    noCleanData: true
});
assert.neq(null, standalone, "Fail to restart the node as standalone");

// Verify that the inserted document does not exist both in the 'stockPrice' and
// the 'system.change_collection' but exists in the 'oplog.rs'.
assert.eq(assert
              .commandWorked(standalone.getDB("test").runCommand(
                  {find: "stockPrice", filter: {_id: "mdb", price: 250}, $tenant: tenantId}))
              .cursor.firstBatch.length,
          0);
assert.eq(assert
              .commandWorked(standalone.getDB("local").runCommand(
                  {find: "oplog.rs", filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}}}))
              .cursor.firstBatch.length,
          1);
assert.eq(assert
              .commandWorked(standalone.getDB("config").runCommand({
                  find: "system.change_collection",
                  filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}},
                  $tenant: tenantId
              }))
              .cursor.firstBatch.length,
          0);

// Stop the mongoD instance and do not clean the db directory.
MongoRunner.stopMongod(standalone, null, {noCleanData: true, skipValidation: true, wait: true});

// Start the replica set primary with the same db path.
replSetTest.start(primary, {
    noCleanData: true,
    serverless: true,
    setParameter: {
        featureFlagServerlessChangeStreams: true,
        multitenancySupport: true,
        featureFlagMongoStore: true,
        featureFlagRequireTenantID: true
    }
});

primary = replSetTest.getPrimary();

// Verify that the 'stockPrice' and the 'system.change_collection' now have the inserted
// document. This document was inserted by applying oplog entries during the startup recovery.
assert.eq(assert
              .commandWorked(primary.getDB("test").runCommand(
                  {find: "stockPrice", filter: {_id: "mdb", price: 250}, $tenant: tenantId}))
              .cursor.firstBatch.length,
          1);
assert.eq(assert
              .commandWorked(primary.getDB("config").runCommand({
                  find: "system.change_collection",
                  filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}},
                  $tenant: tenantId
              }))
              .cursor.firstBatch.length,
          1);

// Get the oplog timestamp up to this point. All oplog entries upto this timestamp must exist in the
// change collection.
const endTimestamp = primary.getDB("local").oplog.rs.find().toArray().at(-1).ts;
assert(endTimestamp !== undefined);

// Verify that the oplog and the change collection entries between the ('startTimestamp',
// 'endTimestamp'] window are exactly same and in the same order.
verifyChangeCollectionEntries(primary, startTimestamp, endTimestamp, tenantId);

replSetTest.stopSet();
})();
