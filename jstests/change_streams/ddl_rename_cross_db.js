/**
 * Tests the change stream event sequence for a cross database rename operation.
 * @tags: [
 *  requires_fcv_60,
 *  # The cross db rename may not always succeed on sharded clusters if they are on different shard.
 *  assumes_against_mongod_not_mongos,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");

const sourceDB = db.getSiblingDB(jsTestName());
const targetDB = db.getSiblingDB(jsTestName() + "_target");

const cst = new ChangeStreamTest(sourceDB.getSiblingDB("admin"));

const collName = "test";
const sourceColl = sourceDB[collName];

function runTest() {
    assert.commandWorked(targetDB[collName].insert({_id: 0}));
    assert.commandWorked(targetDB[collName].insert({_id: 1}));

    assert.commandWorked(sourceColl.insert({_id: 2}));
    assert.commandWorked(sourceColl.insert({_id: 3}));
    assert.commandWorked(sourceColl.update({_id: 3}, {$set: {p: 3}}));

    let cursor = cst.startWatchingAllChangesForCluster();

    assert.commandWorked(sourceDB.adminCommand({
        renameCollection: coll.getFullName(),
        to: targetDB[collName].getFullName(),
        dropTarget: true
    }));

    // Verify that all the documents are cloned into a temporary collection using insert operations.
    // Then the temporary collection is renamed to the target collection before getting dropped.
    //
    // We should see 4 different events for this, two inserts to clone the collection, then rename
    // and drop of the temporary collection.
    const changes = cst.getNextChanges(cursor, 4, false);

    assert.eq(changes[0].operationType, "insert", changes);
    assert.eq(changes[0].documentKey, {_id: 2}, changes);
    assert(changes[0].ns.coll.match(/tmp.*renameCollection/), changes);

    assert.eq(changes[1].operationType, "insert", changes);
    assert.eq(changes[1].documentKey, {_id: 3}, changes);
    assert.eq(changes[1].fullDocument, {_id: 3, p: 3}, changes);
    assert(changes[1].ns.coll.match(/tmp.*renameCollection/), changes);

    assert.eq(changes[2].operationType, "rename", changes);
    assert.eq(changes[2].to, {db: targetDB.getName(), coll: collName}, changes);
    assert(changes[2].ns.coll.match(/tmp.*renameCollection/), changes);

    assert.eq(changes[3].operationType, "drop", changes);
    assert.eq(changes[3].ns, {db: sourceDB.getName(), coll: collName}, changes);
}

runTest();

cst.cleanUp();
}());
