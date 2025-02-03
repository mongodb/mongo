/**
 * Tests that wildcard indexes are prepared to handle and retry WriteConflictExceptions while
 * interacting with the storage layer to retrieve multikey paths.
 *
 * TODO SERVER-56443: This test is specific to the classic engine. If/when the classic engine is
 * deleted, this test should be removed as well.
 */
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
const testDB = conn.getDB("test");

// TODO SERVER-94613: Reenable this test once the stale reference issue is fixed. Early exiting
// in the meantime.
const debugBuild = testDB.adminCommand('buildInfo').debug;
if (debugBuild) {
    jsTest.log("Skipping test as debug builds are susceptible to a consistent collection bug");
    MongoRunner.stopMongod(conn);
    quit();
}

if (checkSbeFullyEnabled(testDB)) {
    jsTestLog("Skipping test as SBE is not resilient to WCEs");
    MongoRunner.stopMongod(conn);
    quit();
}

const coll = testDB.write_conflict_wildcard;
coll.drop();

assert.commandWorked(coll.createIndex({"$**": 1}));

assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: 'WTWriteConflictExceptionForReads', mode: {activationProbability: 0.01}}));
for (let i = 0; i < 1000; ++i) {
    // Insert documents with a couple different multikey paths to increase the number of records
    // scanned during multikey path computation in the wildcard index.
    assert.commandWorked(coll.insert({
        _id: i,
        i: i,
        a: [{x: i - 1}, {x: i}, {x: i + 1}],
        b: [],
        longerName: [{nested: [1, 2]}, {nested: 4}]
    }));
    assert.eq(coll.find({i: i}).hint({"$**": 1}).itcount(), 1);
    if (i > 0) {
        assert.eq(coll.find({"a.x": i}).hint({"$**": 1}).itcount(), 2);
    }
}

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'WTWriteConflictExceptionForReads', mode: "off"}));
MongoRunner.stopMongod(conn);
