// Tests the replSetStepUp command.

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {verifyServerStatusElectionReasonCounterChange} from "jstests/replsets/libs/election_metrics.js";

let name = "stepup";
let rst = new ReplSetTest({name: name, nodes: 2});

rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let secondary = rst.getSecondary();
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

const initialSecondaryStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));

// Step up the primary. Return OK because it's already the primary.
let res = primary.adminCommand({replSetStepUp: 1});
assert.commandWorked(res);
assert.eq(primary, rst.getPrimary());

// Step up the secondary, but it's not eligible to be primary.
// Enable fail point on secondary.
stopServerReplication(secondary);

assert.commandWorked(primary.getDB("test").bar.insert({x: 2}, {writeConcern: {w: 1}}));
res = secondary.adminCommand({replSetStepUp: 1});
assert.commandFailedWithCode(res, ErrorCodes.CommandFailed);
restartServerReplication(secondary);

// Wait for the secondary to catch up by replicating a doc to both nodes.
assert.commandWorked(primary.getDB("test").bar.insert({x: 3}, {writeConcern: {w: "majority"}}));

// Step up the secondary. Retry since the old primary may step down when we try to ask for its
// vote.
let numStepUpCmds = 0;
rst.awaitReplication();
assert.soonNoExcept(function () {
    numStepUpCmds++;
    return secondary.adminCommand({replSetStepUp: 1}).ok;
});

// Make sure the step up succeeded.
assert.eq(secondary, rst.getPrimary());

const newSecondaryStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));

// Check that both the 'called' and 'successful' fields of stepUpCmd have been incremented in
// serverStatus, and that they have not been incremented in any of the other election reason
// counters.
verifyServerStatusElectionReasonCounterChange(
    initialSecondaryStatus.electionMetrics,
    newSecondaryStatus.electionMetrics,
    "stepUpCmd",
    numStepUpCmds /* expectedNumCalled */,
    1 /* expectedNumSuccessful */,
);
verifyServerStatusElectionReasonCounterChange(
    initialSecondaryStatus.electionMetrics,
    newSecondaryStatus.electionMetrics,
    "priorityTakeover",
    0,
);
verifyServerStatusElectionReasonCounterChange(
    initialSecondaryStatus.electionMetrics,
    newSecondaryStatus.electionMetrics,
    "catchUpTakeover",
    0,
);
verifyServerStatusElectionReasonCounterChange(
    initialSecondaryStatus.electionMetrics,
    newSecondaryStatus.electionMetrics,
    "electionTimeout",
    0,
);
verifyServerStatusElectionReasonCounterChange(
    initialSecondaryStatus.electionMetrics,
    newSecondaryStatus.electionMetrics,
    "freezeTimeout",
    0,
);

rst.stopSet();
