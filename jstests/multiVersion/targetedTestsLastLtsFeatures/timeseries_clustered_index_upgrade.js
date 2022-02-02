/**
 * Ensures that it is safe to upgrade a timeseries collection from v5.0 to "latest" without
 * breaking the output of listCollections for the clusteredIndex option. Additionally, that the
 * collection can then be safely downgraded again.
 *
 * TODO SERVER-60219: Since this test is specific to the upgrade process from 5.0 - 5.*, it can be
 * removed once 6.0 becomes last-lts.
 */
(function() {
'use strict';
load('jstests/multiVersion/libs/multi_rs.js');
const tsCollName = 'tsColl';
const bucketsCollName = 'system.buckets.' + tsCollName;

const getTestDB = function(rst) {
    return rst.getPrimary().getDB("test");
};

// The clusteredIndex option for a time-series buckets collection should always be {clusteredIndex:
// true}, regardless of v5.0 vs v5.*.
const assertListCollectionsBucketOutput = function(db) {
    const listColls =
        assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: bucketsCollName}}));
    const options = listColls.cursor.firstBatch[0].options;
    assert(options.clusteredIndex);
    assert.eq(options.clusteredIndex, true);
};

const oldVersion = "last-lts";
const nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion},
    n3: {binVersion: oldVersion}
};

const rst = new ReplSetTest({nodes: nodes});

rst.startSet();
rst.initiate();

let testDB = getTestDB(rst);

assert.commandWorked(testDB.createCollection(tsCollName, {timeseries: {timeField: 'time'}}));
assertListCollectionsBucketOutput(testDB);

jsTest.log("Upgrading replica set from last-lts to latest");
rst.upgradeSet({binVersion: "latest"});

testDB = getTestDB(rst);
assertListCollectionsBucketOutput(testDB);

jsTest.log("Downgrading replica set from latest to last-lts");
rst.upgradeSet({binVersion: "last-lts"});
testDB = getTestDB(rst);
assertListCollectionsBucketOutput(testDB);

rst.stopSet();
})();
