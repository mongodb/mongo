/**
 * Test that vectoredinserts do not hold open an oplog hole; other operations can continue to
 * replicate while a vectored insert is in progress.
 *
 * @tags: [featureFlagReplicateVectoredInsertsTransactionally, requires_timeseries]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
const testName = TestData.testName;
const dbName = testName;
// Set up a two-node replica set
const replTest = new ReplSetTest(
    {name: testName, nodes: [{}, {rsConfig: {priority: 0}}], settings: {chainingAllowed: false}});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();

const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const secondary = replTest.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const collName1 = "testcoll";
const primaryColl1 = primaryDB[collName1];
const secondaryColl1 = secondaryDB[collName1];
const collName2 = "testcoll2";
const primaryColl2 = primaryDB[collName2];

function runOtherOperations(collName) {
    const primaryColl = primaryDB[collName];
    const secondaryColl = secondaryDB[collName];
    jsTestLog("Attempting an insert in " + primaryColl.getFullName());
    assert.commandWorked(
        primaryColl.insert({_id: 0, a: 1}, {writeConcern: {w: 2, wtimeout: 30000}}));
    assert.docEq(secondaryColl.find({a: 1}).toArray(), [{_id: 0, a: 1}]);

    jsTestLog("Attempting a multiple insert in " + primaryColl.getFullName());
    assert.commandWorked(primaryColl.insert([{_id: 1, a: 2}, {_id: 2, a: 2}],
                                            {writeConcern: {w: 2, wtimeout: 30000}}));
    assert.docEq(secondaryColl.find({a: 2}).sort({_id: 1}).toArray(),
                 [{_id: 1, a: 2}, {_id: 2, a: 2}]);

    jsTestLog("Attempting an update in " + primaryColl.getFullName());
    assert.commandWorked(
        primaryColl.update({_id: 0}, {$set: {x: 1}}, {writeConcern: {w: 2, wtimeout: 30000}}));
    assert.docEq(secondaryColl.find({a: 1}).toArray(), [{_id: 0, a: 1, x: 1}]);

    jsTestLog("Attempting an delete in " + primaryColl.getFullName());
    assert.commandWorked(primaryColl.remove({_id: 1}, {writeConcern: {w: 2, wtimeout: 30000}}));
    assert.docEq(secondaryColl.find({a: 2}).toArray(), [{_id: 2, a: 2}]);
}

jsTestLog("Starting blocked vectored insert in parallel thread");
let failPoint = configureFailPoint(primaryDB,
                                   "hangAfterCollectionInserts",
                                   {collectionNS: primaryColl1.getFullName(), first_id: "i0"});
let insertThread = new Thread((host, dbName, collName) => {
    const db = new Mongo(host).getDB(dbName);
    assert.commandWorked(
        db[collName].insert([{_id: "i0", a: 0}, {_id: "i1", a: 0}], {writeConcern: {w: 2}}));
}, primary.host, dbName, collName1);
insertThread.start();
failPoint.wait();

jsTestLog("Trying operations on a collection not being inserted into.");
runOtherOperations(collName2);

jsTestLog("Trying operations on the same collection as the insert.");
runOtherOperations(collName1);

jsTestLog("Allowing insert to finish");
failPoint.off();
insertThread.join();

assert.docEq(secondaryColl1.find({a: 0}).sort({_id: 1}).toArray(),
             [{_id: "i0", a: 0}, {_id: "i1", a: 0}]);

primaryColl2.drop();

jsTestLog("Creating timeseries collection");
const collNameTS = "tstestcoll";
const primaryCollTS = primaryDB[collNameTS];
const primaryBucketsCollTS = primaryDB["system.buckets." + collNameTS];
assert.commandWorked(primaryDB.createCollection(
    primaryCollTS.getName(), {timeseries: {timeField: "time", metaField: "measurement"}}));

jsTestLog("Starting blocked vectored time series insert in parallel thread");
failPoint = configureFailPoint(
    primaryDB, "hangAfterCollectionInserts", {collectionNS: primaryBucketsCollTS.getFullName()});
insertThread = new Thread((host, dbName, collName) => {
    const db = new Mongo(host).getDB(dbName);
    const objA = {"time": ISODate("2021-01-01T01:00:00Z"), "measurement": "A"};
    const objB = {"time": ISODate("2021-01-01T01:01:00Z"), "measurement": "B"};
    assert.commandWorked(db[collName].insert([objA, objB], {writeConcern: {w: 2}}));
}, primary.host, dbName, collNameTS);
insertThread.start();
failPoint.wait();

jsTestLog("Trying operations on a collection other than the timeseries collection.");
runOtherOperations(collName2);

// We do not test operations on the same timeseries collection at the same time as the failpoint
// would cause that to hang.
failPoint.off();
insertThread.join();

replTest.stopSet();
