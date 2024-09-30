/**
 * Test that verifies durationMillis and workingMillis are accepted profile filter fields.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const primary = rst.getPrimary();
const db = primary.getDB(dbName);

function runCommandAndCheckLog(collName, shouldFindLog) {
    // Use unique collection names so slow query logs are identifiable by test case.
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));

    let predicate = new RegExp(`Slow query.*"` + collName);
    if (shouldFindLog) {
        assert(checkLog.checkContainsOnce(primary, predicate),
               "Could not find log containing " + predicate);
    } else {
        assert.neq(checkLog.checkContainsOnce(primary, predicate),
                   "Could not find log containing " + predicate);
    }
}

/**
 * Test case 1: Default profiling settings uses workingMillis. Increase slowMs threshold to 10000ms
 * to check that no slow query log is produced.
 */
assert.commandWorked(db.setProfilingLevel(0, 10000));
runCommandAndCheckLog("case1", /*shouldFindLog*/ false);

/** Test case 2: Lower slowMs threshold to -1 so all operations should produce slow query log.*/
assert.commandWorked(db.setProfilingLevel(0, -1));
runCommandAndCheckLog("case2", /*shouldFindLog*/ true);

/**
 * Test case 3: Set profile filter to any operation where durationMillis is greater than -1. All
 * operations should produce slow query log.
 */
// Reset profiling level and check no log is produced.
assert.commandWorked(db.setProfilingLevel(0, 10000));
runCommandAndCheckLog("case3reset", /*shouldFindLog*/ false);

assert.commandWorked(db.setProfilingLevel(0, {filter: {durationMillis: {$gt: -1}}}));
runCommandAndCheckLog("case3", /*shouldFindLog*/ true);

/**
 * Test case 4: Set profile filter to any operation where workingMillis is greater than -1. All
 * operations should produce slow query log.
 */
// Unset filter and check no log is produced.
assert.commandWorked(db.setProfilingLevel(0, {filter: "unset"}));
runCommandAndCheckLog("case4reset", /*shouldFindLog*/ false);

assert.commandWorked(db.setProfilingLevel(0, {filter: {workingMillis: {$gt: -1}}}));
runCommandAndCheckLog("case4", /*shouldFindLog*/ true);

rst.stopSet();
