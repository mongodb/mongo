/**
 * decreasing_election_timeout.js
 *
 * Test that election timeout can be decreased via reconfig and that the election timeout callback
 * is rescheduled accordingly.
 *
 * Steps:
 * 1. Start a 2-node replica set with a large electionTimeoutMillis (default) and election log
 *    verbosity set to 5.
 * 2. Wait for log: "Scheduled election timeout callback" (should be far in the future).
 * 3. Reconfig to set electionTimeoutMillis to 1000 ms (1 second).
 * 4. Wait for logs:
 *    - "Canceling election timeout callback and not rescheduling"
 *    - "Rescheduled election timeout callback"
 * 5. Step down primary with force:true.
 * 6. Wait for secondary to become primary (should be <2s).
 *
 * @tags: [requires_replication]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {logComponentVerbosity: tojson({replication: 5})},
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

// Log the initial election timeout (set to default high value).
const initialConfig = rst.getReplSetConfigFromNode();
const initialElectionTimeout = initialConfig.settings.electionTimeoutMillis;
jsTestLog("Initial electionTimeoutMillis is set to default high value: " + initialElectionTimeout +
          "ms");

jsTestLog("Waiting for 'Scheduled election timeout callback' log on secondary");
checkLog.containsWithCount(secondary, "Scheduled election timeout callback", 1);
jsTestLog("Saw 'Scheduled election timeout callback' log");

// Reconfig to decrease electionTimeoutMillis to 1 second
jsTestLog("Reconfiguring to set electionTimeoutMillis to 1000ms (down from " +
          initialElectionTimeout + "ms)");
let config = rst.getReplSetConfigFromNode();
config.version++;
config.settings = config.settings || {};
config.settings.electionTimeoutMillis = 1000;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

jsTestLog("Waiting for 'Canceling election timeout callback' log");
checkLog.containsWithAtLeastCount(
    secondary,
    "Moving a delayable timeout call backwards, which requires canceling and rescheduling",
    1,
);
jsTestLog("Saw 'Canceling election timeout callback' log");

// Step down primary with force:true so election handoff won't work.
// The 600 second stepdown period prevents the old primary from running for election.
jsTestLog("Stepping down primary with force:true");
const oldPrimary = primary;
assert.commandWorked(oldPrimary.adminCommand({replSetStepDown: 600, force: true}));

// Wait for secondary to become primary (should be <2s with 1s election timeout)
jsTestLog("Waiting for secondary to become primary");
rst.awaitNodesAgreeOnPrimary();
const newPrimary = rst.getPrimary();
assert.eq(newPrimary.host, secondary.host, "Expected secondary to become primary");
jsTestLog("Secondary became primary successfully");

rst.stopSet();
