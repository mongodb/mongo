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

function validateCollections() {
    assert.eq(3, db.unshardedFoo.count());
    assert.eq(3, db.shardedBar.count());
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
        assert.commandWorked(db.unshardedFoo.insert({a: i}));
        assert.commandWorked(db.shardedBar.insert({a: i}));
    }

    assert.commandWorked(
        db.runCommand({createIndexes: 'unshardedFoo', indexes: unshardedFooIndexes}));
    assert.commandWorked(db.runCommand({createIndexes: 'shardedBar', indexes: shardedBarIndexes}));

    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db.adminCommand({shardCollection: dbName + '.shardedBar', key: {_id: 1}}));
}

// start move primary and hang during cloneCatalog
// failViaMongos indicates if mongos can get an up to date routing info
function testMovePrimary(failpoint, fromShard, toShard, coll, shouldFail) {
    let codeToRunInParallelShell = '{ db.getSiblingDB("admin").runCommand({movePrimary: "' +
        dbName + '", to: "' + toShard.name + '"}); }';

    assert.commandWorked(fromShard.adminCommand({configureFailPoint: failpoint, mode: 'alwaysOn'}));

    let awaitShell = startParallelShell(codeToRunInParallelShell, st.s.port);

    jsTestLog("Waiting for failpoint " + failpoint);
    waitForFailpoint("Hit " + failpoint, 1);
    clearRawMongoProgramOutput();

    // Test DML
    jsTestLog("Before insert");
    if (shouldFail) {
        assert.commandFailedWithCode(coll.insert({a: 10}), ErrorCodes.MovePrimaryInProgress);
    } else {
        assert.commandWorked(coll.insert({a: 10}));
    }

    jsTestLog("Before update");
    if (shouldFail) {
        assert.commandFailedWithCode(coll.update({a: 1}, {$set: {a: 10}}),
                                     ErrorCodes.MovePrimaryInProgress);
    } else {
        assert.commandWorked(coll.update({a: 11}, {$set: {a: 11}}));
    }

    jsTestLog("Before remove");
    if (shouldFail) {
        assert.commandFailedWithCode(coll.remove({a: 1}), ErrorCodes.MovePrimaryInProgress);
    } else {
        assert.commandWorked(coll.remove({a: 11}));
    }

    assert.commandWorked(fromShard.adminCommand({configureFailPoint: failpoint, mode: 'off'}));

    awaitShell();
}

createCollections();
let fromShard = st.getPrimaryShard(dbName);
let toShard = st.getOther(fromShard);

testMovePrimary('hangInCloneStage', fromShard, toShard, fromShard.getDB(dbName).shardedBar, false);

createCollections();
fromShard = st.getPrimaryShard(dbName);
toShard = st.getOther(fromShard);
testMovePrimary(
    'hangInCleanStaleDataStage', fromShard, toShard, fromShard.getDB(dbName).shardedBar, false);

st.stop();
})();
