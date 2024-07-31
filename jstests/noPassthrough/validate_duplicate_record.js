/**
 * Tests that duplicate records for _id index keys are detected by validate.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

// Disable testing diagnostics (TestingProctor) so we do not hit test only fasserts.
TestData.testingDiagnosticsEnabled = false;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getCollection('test.duplicate_record');
assert.commandWorked(coll.createIndex({x: 1}));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

function testValidateWithFailpoint(fpName) {
    assert.commandWorked(primary.adminCommand({configureFailPoint: fpName, mode: "alwaysOn"}));
    let res = assert.commandWorked(coll.validate());
    assert(!res.valid);
    assert.commandWorked(primary.adminCommand({configureFailPoint: fpName, mode: "off"}));
}

// Test duplicate record for index key on _id index.
testValidateWithFailpoint("WTIndexUassertDuplicateRecordForIdIndex");

rst.stopSet();
})();
