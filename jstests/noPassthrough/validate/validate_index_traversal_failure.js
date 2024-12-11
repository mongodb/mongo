/**
 * Tests that index validation completes upon index traversal failure
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getCollection('test.traversal_failure');
assert.commandWorked(coll.createIndex({x: 1}));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Test that validation runs to completion when encountering a index traversal issue
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failIndexTraversal", mode: "alwaysOn"}));
let res = assert.commandWorked(coll.validate());
jsTestLog(res);
assert(!res.valid);
assert.eq(0, res.missingIndexEntries.length);
assert.eq(0, res.extraIndexEntries.length);
// We don't expect a warning about inconsistent counts for indexes. This can happen even if the
// above asserts pass if we fail to skip the second phase of validation for broken indexes
assert.eq(0, res.warnings.length);
assert.commandWorked(primary.adminCommand({configureFailPoint: "failIndexTraversal", mode: "off"}));

rst.stopSet();
