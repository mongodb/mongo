/**
 * Tests that the _shardsvrDropCollectionIfUUIDNotMatching and
 * _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern command works as expected:
 * - Noop in case the collection doesn't exist.
 * - Drop collection if uuid different from the expected.
 * - Keep the collection if the uuid is exactly the expected one.
 *
 * @tags: [
 *     does_not_support_stepdowns, # The command is not resilient to stepdowns
 * ]
 */

"use strict";

const dbName = "test";

const st = new ShardingTest({shards: 1});
const mongos = st.s;
db = st.rs0.getPrimary().getDB(dbName);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

function appenWriteConcern(command, writeConcern) {
    if (writeConcern) {
        Object.assign(command, writeConcern);
        return command;
    } else {
        return command;
    }
}

function runTests(collName, commandName, writeConcern) {
    const ns = dbName + "." + collName;
    // Non-existing collection with a random expected UUID (command succeeds, noop)
    assert.commandWorked(db.runCommand(appenWriteConcern(
        {[`${commandName}`]: collName, expectedCollectionUUID: UUID()}, writeConcern)));

    // Existing collection with a random expected UUID (command succeeds after successful drop)
    assert.commandWorked(db.getCollection(collName).insert({_id: 0}));
    assert.commandWorked(db.runCommand(appenWriteConcern(
        {[`${commandName}`]: collName, expectedCollectionUUID: UUID()}, writeConcern)));
    assert.eq(null, db.getCollection(collName).findOne({_id: 0}));

    // Existing collection with the expected UUID (command succeeds but no drop)
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    const collUUID = st.config.collections.findOne({_id: ns}).uuid;
    assert.commandWorked(db.getCollection(collName).insert({_id: 0}));
    assert.commandWorked(db.runCommand(appenWriteConcern(
        {[`${commandName}`]: collName, expectedCollectionUUID: collUUID}, writeConcern)));
    assert.neq(null, db.getCollection(collName).findOne({_id: 0}));
}

// TODO SERVER-74324: deprecate _shardsvrDropCollectionIfUUIDNotMatching after 7.0 is lastLTS.
runTests("coll", "_shardsvrDropCollectionIfUUIDNotMatching", null);
if (!jsTestOptions().shardMixedBinVersions && jsTestOptions().mongosBinVersion == "latest") {
    runTests("coll2",
             "_shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern",
             {writeConcern: {w: 'majority'}});
}

st.stop();
