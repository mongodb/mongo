/**
 * Tests that replSetInitiate eventually succeeds despite temporary DNS outage.
 *
 * @tags: [
 *   disabled_due_to_server_58295
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
const nodes = rst.startSet();
// Node 1 will fail to find itself in the config.
const failPoint = configureFailPoint(nodes[1], "failIsSelfCheck");
const config = rst.getReplSetConfig();
assert.commandWorked(nodes[0].adminCommand({replSetInitiate: config}));
failPoint.wait();
assert.commandFailedWithCode(nodes[1].adminCommand({replSetGetStatus: 1}),
                             ErrorCodes.NotYetInitialized);
failPoint.off();
// Node 1 re-checks isSelf on next heartbeat and succeeds.
waitForState(nodes[1], ReplSetTest.State.SECONDARY);
rst.stopSet();
})();
