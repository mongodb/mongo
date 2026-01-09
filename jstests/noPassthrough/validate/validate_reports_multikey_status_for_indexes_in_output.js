/**
 * Tests that validate reports whether or not an index is multikey in the 'indexDetails' field of its output.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "test";
const primary = rst.getPrimary();
const testColl = primary.getDB(dbName).getCollection(collName);

// Non-multikey index.
assert.commandWorked(testColl.insert({x: 1}));
assert.commandWorked(testColl.createIndex({x: 1}));

// Multikey index.
assert.commandWorked(testColl.insert({a: [1, 2, 3]}));
assert.commandWorked(testColl.createIndex({a: 1}));

const result = assert.commandWorked(testColl.validate({full: true}));
assert.eq(testColl.getFullName(), result.ns, tojson(result));
assert(result.valid, tojson(result));
assert.eq(false, result.indexDetails.x_1.isMultikey, tojson(result));
assert.eq(true, result.indexDetails.a_1.isMultikey, tojson(result));

rst.stopSet();
