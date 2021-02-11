/**
 * Tests that $bit updates succeed if primaries compare updates numerically and secondaries compare
 * updates lexicographically for array indexes.
 */

(function() {
"use strict";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {binVersion: "latest"},
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB(jsTestName());
const coll = testDB.test;

testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV});

assert.commandWorked(coll.insert({_id: 0, arr: [0]}));

assert.commandWorked(
    coll.update({_id: 0}, {$bit: {"a.9": {or: NumberInt(0)}, "a.10": {or: NumberInt(0)}}}));

rst.stopSet();
})();
