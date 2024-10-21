/**
 * Tests that test-only replica-set only commands are truly test-only.
 *
 * @tags: [
 *   # TODO (SERVER-80568): Re-enable this test in multiversion suites once it has been fixed.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 *   disables_test_commands,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const cmdList = [
    {'replSetGetConfig': 1, '$_internalIncludeNewlyAdded': true},
    {'replSetGetConfig': 1, '$_internalIncludeNewlyAdded': false}
];

TestData.enableTestCommands = false;
let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate(null, "replSetInitiate", {doNotWaitForNewlyAddedRemovals: true});

let primary = rst.getPrimary();
for (let cmd of cmdList) {
    assert.commandFailedWithCode(primary.adminCommand(cmd), ErrorCodes.InvalidOptions);
}

rst.awaitReplication();
rst.stopSet();

TestData.enableTestCommands = true;
rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate(null, "replSetInitiate", {doNotWaitForNewlyAddedRemovals: true});

primary = rst.getPrimary();
for (let cmd of cmdList) {
    assert.commandWorked(primary.adminCommand(cmd));
}

rst.awaitReplication();
rst.stopSet();
