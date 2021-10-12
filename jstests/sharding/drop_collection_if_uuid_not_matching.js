/**
 * Tests that the _shardsvrDropCollectionIfUUIDNotMatching command works as expected:
 * - Noop in case the collection doesn't exist.
 * - Drop collection if uuid different from the expected.
 * - Keep the collection if the uuid is exactly the expected one.
 *
 * @tags: [
 *     requires_fcv_50, // The command is not present in v4.4
 *     does_not_support_stepdowns, // The command is not resilient to stepdowns
 * ]
 */

"use strict";

const dbName = "test";
const collName = "coll";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 1});
const mongos = st.s;
db = st.rs0.getPrimary().getDB(dbName);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

// Non-existing collection with a random expected UUID (command succeeds, noop)
assert.commandWorked(db.runCommand(
    {_shardsvrDropCollectionIfUUIDNotMatching: collName, expectedCollectionUUID: UUID()}));

// Existing collection with a random expected UUID (command succeeds after successful drop)
assert.commandWorked(db.getCollection(collName).insert({_id: 0}));
assert.commandWorked(db.runCommand(
    {_shardsvrDropCollectionIfUUIDNotMatching: collName, expectedCollectionUUID: UUID()}));
assert.eq(null, db.getCollection(collName).findOne({_id: 0}));

// Existing collection with the expected UUID (command succeeds but no drop)
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
const collUUID = st.config.collections.findOne({_id: ns}).uuid;
assert.commandWorked(db.getCollection(collName).insert({_id: 0}));
assert.commandWorked(db.runCommand(
    {_shardsvrDropCollectionIfUUIDNotMatching: collName, expectedCollectionUUID: collUUID}));
assert.neq(null, db.getCollection(collName).findOne({_id: 0}));

st.stop();
