/**
 * Tests that out-of-order keys are detected by validation during both the collection and index scan
 * phases.
 *
 * @tags: [requires_wiredtiger]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getCollection('test.out_of_order');
assert.commandWorked(coll.createIndex({x: 1}));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Test record store out-of-order detection.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "WTRecordStoreUassertOutOfOrder", mode: "alwaysOn"}));
let res = assert.commandWorked(coll.validate());
assert(!res.valid);
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "WTRecordStoreUassertOutOfOrder", mode: "off"}));

// Ensure that $collStats info gets logged when validation fails.
checkLog.containsJson(primary, 7463200);

// Test index entry out-of-order detection.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "alwaysOn"}));
res = assert.commandWorked(coll.validate());
assert(!res.valid);
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "off"}));

rst.stopSet();
})();
