/**
 * The failpoints used here are not defined in the previous release (4.4).
 * @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

let st = new ShardingTest({
    mongos: 2,
    shards: 2,
});

const dbName = "testdb";

function verifyDocuments(db, count) {
    assert.eq(count, db.unshardedFoo.count());
}

function createCollections() {
    assert.commandWorked(st.getDB(dbName).runCommand({dropDatabase: 1}));
    let db = st.getDB(dbName);

    const unshardedFooIndexes = [{key: {a: 1}, name: 'fooIndex'}];
    const shardedBarIndexes = [{key: {a: 1}, name: 'barIndex'}];

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

    assert.commandWorked(db.createCollection('unshardedFoo'));
    assert.commandWorked(db.createCollection('shardedBar'));

    for (let i = 0; i < 3; i++) {
        assert.commandWorked(db.unshardedFoo.insert({_id: i, a: i}));
        assert.commandWorked(db.shardedBar.insert({_id: i, a: i}));
    }

    assert.commandWorked(
        db.runCommand({createIndexes: 'unshardedFoo', indexes: unshardedFooIndexes}));
    assert.commandWorked(db.runCommand({createIndexes: 'shardedBar', indexes: shardedBarIndexes}));

    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db.adminCommand({shardCollection: dbName + '.shardedBar', key: {_id: 1}}));
}

function buildCommands(collName) {
    const commands = [
        {insert: collName, documents: [{a: 10}]},
        {update: collName, updates: [{q: {a: 1}, u: {$set: {a: 11}}}]},
        {findAndModify: collName, query: {a: 2}, update: {$set: {a: 11}}},
        {delete: collName, deletes: [{q: {a: 0}, limit: 1}]}
    ];
    return commands;
}

function testMovePrimary(failpoint, fromShard, toShard, db, shouldFail, sharded) {
    let codeToRunInParallelShell = '{ db.getSiblingDB("admin").runCommand({movePrimary: "' +
        dbName + '", to: "' + toShard.name + '"}); }';

    assert.commandWorked(fromShard.adminCommand({configureFailPoint: failpoint, mode: 'alwaysOn'}));

    let awaitShell = startParallelShell(codeToRunInParallelShell, st.s.port);

    jsTestLog("Waiting for failpoint " + failpoint);
    waitForFailpoint("Hit " + failpoint, 1);
    clearRawMongoProgramOutput();

    // Test DML

    let collName;
    if (sharded) {
        collName = "shardedBar";
    } else {
        collName = "unshardedFoo";
    }

    buildCommands(collName).forEach(command => {
        if (shouldFail) {
            jsTestLog("running command: " + tojson(command) + ",\nshoudFail: " + shouldFail);
            assert.commandFailedWithCode(db.runCommand(command), ErrorCodes.MovePrimaryInProgress);
        } else {
            jsTestLog("running command: " + tojson(command) + ",\nshoudFail: " + shouldFail);
            assert.commandWorked(db.runCommand(command));
        }
    });

    assert.commandWorked(fromShard.adminCommand({configureFailPoint: failpoint, mode: 'off'}));

    awaitShell();
}

createCollections();
let fromShard = st.getPrimaryShard(dbName);
let toShard = st.getOther(fromShard);

testMovePrimary('hangInCloneStage', fromShard, toShard, fromShard.getDB(dbName), true, false);
verifyDocuments(toShard.getDB(dbName), 3);
verifyDocuments(fromShard.getDB(dbName), 0);

createCollections();
fromShard = st.getPrimaryShard(dbName);
toShard = st.getOther(fromShard);
testMovePrimary('hangInCloneStage', fromShard, toShard, fromShard.getDB(dbName), false, true);

createCollections();
fromShard = st.getPrimaryShard(dbName);
toShard = st.getOther(fromShard);
testMovePrimary('hangInCloneStage', fromShard, toShard, st.s.getDB(dbName), true, false);
verifyDocuments(toShard.getDB(dbName), 3);
verifyDocuments(fromShard.getDB(dbName), 0);

createCollections();
fromShard = st.getPrimaryShard(dbName);
toShard = st.getOther(fromShard);
testMovePrimary('hangInCleanStaleDataStage', fromShard, toShard, st.s.getDB(dbName), false, false);

st.stop();
})();
