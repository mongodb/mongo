/**
 * Tests that replSetInitiate eventually succeeds despite temporary DNS outage.
 *
 * @tags: [
 *   multiversion_incompatible
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

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
