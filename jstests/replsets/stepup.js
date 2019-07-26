// Tests the replSetStepUp command.

load("jstests/replsets/rslib.js");

(function() {

"use strict";
var name = "stepup";
var rst = new ReplSetTest({name: name, nodes: 2});

rst.startSet();
rst.initiate();
rst.awaitReplication();

var primary = rst.getPrimary();
var secondary = rst.getSecondary();

// Step up the primary. Return OK because it's already the primary.
var res = primary.adminCommand({replSetStepUp: 1});
assert.commandWorked(res);
assert.eq(primary, rst.getPrimary());

// Step up the secondary, but it's not eligible to be primary.
// Enable fail point on secondary.
assert.commandWorked(
    secondary.getDB('admin').runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}));

assert.writeOK(primary.getDB("test").bar.insert({x: 2}, {writeConcern: {w: 1}}));
res = secondary.adminCommand({replSetStepUp: 1});
assert.commandFailedWithCode(res, ErrorCodes.CommandFailed);
assert.commandWorked(
    secondary.getDB('admin').runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}));

// Wait for the secondary to catch up by replicating a doc to both nodes.
assert.writeOK(primary.getDB("test").bar.insert({x: 3}, {writeConcern: {w: "majority"}}));

// Step up the secondary. Retry since the old primary may step down when we try to ask for its
// vote.
assert.soonNoExcept(function() {
    return secondary.adminCommand({replSetStepUp: 1}).ok;
});

// Make sure the step up succeeded.
assert.eq(secondary, rst.getPrimary());

rst.stopSet();
})();
