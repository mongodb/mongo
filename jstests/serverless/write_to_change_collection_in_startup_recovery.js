// Tests that replaying the oplog entries during the startup recovery also writes to the change
// collection.
// @tags: [
//   requires_fcv_62,
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {verifyChangeCollectionEntries} from "jstests/serverless/libs/change_collection_util.js";

const replSetTest =
    new ReplSetTest({nodes: 1, name: "ChangeStreamMultitenantReplicaSetTest", serverless: true});

replSetTest.startSet({
    setParameter: {
        featureFlagServerlessChangeStreams: true,
        multitenancySupport: true,
        featureFlagRequireTenantID: true,
        featureFlagSecurityToken: true
    }
});
replSetTest.initiate();

let primary = replSetTest.getPrimary();
const tenantId = ObjectId();

const token = _createTenantToken({tenant: tenantId});
primary._setSecurityToken(token);

// Enable the change stream to create the change collection.
assert.commandWorked(primary.getDB("admin").runCommand({setChangeStreamState: 1, enabled: true}));

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
    {insert: "stockPrice", documents: [{_id: "mdb", price: 250}]}));

// Verify that the inserted document can be queried from the 'stockPrice', the 'oplog.rs', and
// the 'system.change_collection'.
assert.eq(assert
              .commandWorked(primary.getDB("test").runCommand(
                  {find: "stockPrice", filter: {_id: "mdb", price: 250}}))
              .cursor.firstBatch.length,
          1);

// Clear the token before accessing the local database
primary._setSecurityToken(undefined);
assert.eq(assert
              .commandWorked(primary.getDB("local").runCommand(
                  {find: "oplog.rs", filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}}}))
              .cursor.firstBatch.length,
          1);
primary._setSecurityToken(token);

assert.eq(assert
              .commandWorked(primary.getDB("config").runCommand({
                  find: "system.change_collection",
                  filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}}
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
        featureFlagRequireTenantID: true
    },
    dbpath: primary.dbpath,
    noReplSet: true,
    noCleanData: true
});
assert.neq(null, standalone, "Fail to restart the node as standalone");

// Set the token on the new connection
standalone._setSecurityToken(token);

// Verify that the inserted document does not exist both in the 'stockPrice' and
// the 'system.change_collection' but exists in the 'oplog.rs'.
assert.eq(assert
              .commandWorked(standalone.getDB("test").runCommand(
                  {find: "stockPrice", filter: {_id: "mdb", price: 250}}))
              .cursor.firstBatch.length,
          0);

// Clear the token before accessing the local database
standalone._setSecurityToken(undefined);
assert.eq(assert
              .commandWorked(standalone.getDB("local").runCommand(
                  {find: "oplog.rs", filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}}}))
              .cursor.firstBatch.length,
          1);
standalone._setSecurityToken(token);

assert.eq(assert
              .commandWorked(standalone.getDB("config").runCommand({
                  find: "system.change_collection",
                  filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}}
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
        featureFlagRequireTenantID: true
    }
});

primary = replSetTest.getPrimary();
primary._setSecurityToken(token);

// Verify that the 'stockPrice' and the 'system.change_collection' now have the inserted
// document. This document was inserted by applying oplog entries during the startup recovery.
assert.eq(assert
              .commandWorked(primary.getDB("test").runCommand(
                  {find: "stockPrice", filter: {_id: "mdb", price: 250}}))
              .cursor.firstBatch.length,
          1);
assert.eq(assert
              .commandWorked(primary.getDB("config").runCommand({
                  find: "system.change_collection",
                  filter: {ns: "test.stockPrice", o: {_id: "mdb", price: 250}}
              }))
              .cursor.firstBatch.length,
          1);

// Get the oplog timestamp up to this point. All oplog entries upto this timestamp must exist in the
// change collection.
primary._setSecurityToken(undefined);
const endTimestamp = primary.getDB("local").oplog.rs.find().toArray().at(-1).ts;
assert(endTimestamp !== undefined);

// Verify that the oplog and the change collection entries between the ('startTimestamp',
// 'endTimestamp'] window are exactly same and in the same order.
verifyChangeCollectionEntries(primary, startTimestamp, endTimestamp, tenantId, token);

replSetTest.stopSet();
