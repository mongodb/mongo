/**
 *  Tests that majority reads can complete successfully even when the cluster time is being
 *  increased rapidly while ddl operations are happening.
 *
 *  @tags: [requires_replication]
 */
(function() {
'use strict';
load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

const rst = new ReplSetTest({nodes: 1});
if (!startSetIfSupportsReadMajority(rst)) {
    jsTest.log("skipping test since storage engine doesn't support committed reads");
    rst.stopSet();
    return;
}

rst.initiate();
const primary = rst.getPrimary();
const syncName = 'sync';
const syncColl = primary.getDB(syncName).getCollection(syncName);
assert.commandWorked(syncColl.insert({t: 'before'}));

function bumpClusterTime() {
    jsTestLog('Beginning to bump the logical clock.');
    const syncName = 'sync';
    const syncColl = db.getSiblingDB(syncName).getCollection(syncName);
    assert.eq(syncColl.find().itcount(), 1);
    assert.commandWorked(syncColl.insert({t: 'during'}));
    assert.eq(syncColl.find().itcount(), 2);

    let clusterTime = new Timestamp(1, 1);
    while (true) {
        const higherClusterTime = new Timestamp(clusterTime.getTime() + 20, 1);
        const res = assert.commandWorked(db.adminCommand({
            'isMaster': 1,
            '$clusterTime': {
                'clusterTime': higherClusterTime,
                'signature':
                    {'hash': BinData(0, 'AAAAAAAAAAAAAAAAAAAAAAAAAAA='), 'keyId': NumberLong(0)}
            }
        }));
        clusterTime = res.$clusterTime.clusterTime;

        if (syncColl.find().itcount() === 3) {
            jsTestLog('Done bumping the logical clock.');
            return;
        }
    }
}

const clusterTimeBumper = startParallelShell(bumpClusterTime, primary.port);
// Wait for the logical clock to begin to be bumped.
assert.soon(() => syncColl.find().itcount() === 2);

function doMajorityRead(coll, expectedCount) {
    const res = assert.commandWorked(coll.runCommand('find', {
        'filter': {x: 7},
        'readConcern': {'level': 'majority'},
        'maxTimeMS': rst.kDefaultTimeoutMS
    }));
    // Exhaust the cursor to avoid leaking cursors on the server.
    assert.eq(expectedCount, new DBCommandCursor(coll.getDB(), res).itcount());
}

const dbName = 'minimum_visible_with_cluster_time';
const collName = 'foo';

for (let i = 0; i < 10; i++) {
    const collNameI = collName + i;
    jsTestLog(`Testing ${dbName}.${collNameI}`);

    assert.commandWorked(primary.getDB(dbName).createCollection(collNameI));
    let coll = primary.getDB(dbName).getCollection(collNameI);

    doMajorityRead(coll, 0);

    assert.commandWorked(coll.insert({x: 7, y: 1}));
    assert.commandWorked(
        coll.createIndex({x: 1}, {'name': 'x_1', 'expireAfterSeconds': 60 * 60 * 23}));

    doMajorityRead(coll, 1);

    assert.commandWorked(coll.insert({x: 7, y: 2}));
    assert.commandWorked(coll.runCommand(
        'collMod', {'index': {'keyPattern': {x: 1}, 'expireAfterSeconds': 60 * 60 * 24}}));
    doMajorityRead(coll, 2);

    assert.commandWorked(coll.insert({x: 7, y: 3}));
    assert.commandWorked(coll.dropIndexes());

    doMajorityRead(coll, 3);

    assert.commandWorked(coll.insert({x: 7, y: 4}));
    const newCollNameI = collNameI + '_new';
    assert.commandWorked(coll.renameCollection(newCollNameI));

    coll = primary.getDB(dbName).getCollection(newCollNameI);
    doMajorityRead(coll, 4);
}

jsTestLog('Waiting for logical clock thread to stop.');
assert.commandWorked(syncColl.insert({t: 'after'}));
clusterTimeBumper();

rst.stopSet();
})();