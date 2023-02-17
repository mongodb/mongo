/**
 * Tests that out-of-order keys are detected by validation during both the collection and index scan
 * phases.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getCollection('test.out_of_order');
assert.commandWorked(coll.createIndex({x: 1}));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Disable the journal flusher for the remainder of the test so that it will not encounter the
// out-of-order uassert.
const journalFp = configureFailPoint(primary, "pauseJournalFlusherThread");
journalFp.wait();

// Test record store out-of-order detection.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "WTRecordStoreUassertOutOfOrder", mode: "alwaysOn"}));
let res = assert.commandWorked(coll.validate());
assert(!res.valid);
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "WTRecordStoreUassertOutOfOrder", mode: "off"}));

// Test index entry out-of-order detection.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "alwaysOn"}));
res = assert.commandWorked(coll.validate());
assert(!res.valid);
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "off"}));

rst.stopSet();
})();
