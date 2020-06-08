/**
 * Tests that test-only replica-set only commands are truly test-only.
 *
 * @tags: [
 *   requires_fcv_46,
 * ]
 */

(function() {
"use strict";

const cmdList = [
    {'replSetGetConfig': 1, '$_internalIncludeNewlyAdded': true},
    {'replSetGetConfig': 1, '$_internalIncludeNewlyAdded': false}
];

TestData.enableTestCommands = false;
let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiateWithAnyNodeAsPrimary(null, "replSetInitiate", {doNotWaitForNewlyAddedRemovals: true});

let primary = rst.getPrimary();
for (let cmd of cmdList) {
    assert.commandFailedWithCode(primary.adminCommand(cmd), ErrorCodes.InvalidOptions);
}

rst.awaitReplication();
rst.stopSet();

TestData.enableTestCommands = true;
rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiateWithAnyNodeAsPrimary(null, "replSetInitiate", {doNotWaitForNewlyAddedRemovals: true});

primary = rst.getPrimary();
for (let cmd of cmdList) {
    assert.commandWorked(primary.adminCommand(cmd));
}

rst.awaitReplication();
rst.stopSet();
})();
