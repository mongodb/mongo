/**
 * Test that verifies that workingMillis is logged as part of slow query logging.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "testcoll";
const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const coll = db[collName];

// Set slow threshold to -1 to ensure that all operations are logged as SLOW.
assert.commandWorked(db.setProfilingLevel(1, {slowms: -1}));
assert.commandWorked(coll.insert({a: 1}));

// workingMillis should be present in the slow query log.
const predicate = new RegExp(`Slow query.*"${coll}.*"workingMillis"`);
assert(checkLog.checkContainsOnce(primary, predicate),
       "Could not find log containing " + predicate);

rst.stopSet();
