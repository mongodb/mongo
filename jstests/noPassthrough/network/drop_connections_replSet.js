/**
 * verify dropConnections command works for replica sets
 * @tags: [
 *   requires_replication,
 * ]
 */

import {
    assertHasConnPoolStats,
    checkHostHasNoOpenConnections,
    checkHostHasOpenConnections
} from "jstests/libs/conn_pool_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
rst.awaitSecondaryNodes();

let currentCheckNumber = 0;

// To test the dropConnections command, first remove the secondary. This should have no effect
// on the existing connection pool, but it'll prevent the primary from reconnecting to it after
// dropConnections. Then, execute dropConnections and check that the primary has 0 connections
// to the secondary.
const cfg = primary.getDB('local').system.replset.findOne();
const memberHost = cfg.members[2].host;
currentCheckNumber = assertHasConnPoolStats(
    primary, [memberHost], {checkStatsFunc: checkHostHasOpenConnections}, currentCheckNumber);

const removedMember = cfg.members.splice(2, 1);
assert.eq(removedMember[0].host, memberHost);
cfg.version++;

jsTestLog("Reconfiguring to omit " + memberHost);
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

// Reconfig did not affect the connection pool
currentCheckNumber = assertHasConnPoolStats(
    primary, [memberHost], {checkStatsFunc: checkHostHasOpenConnections}, currentCheckNumber);

// Test dropConnections
jsTestLog("Dropping connections to " + memberHost);
assert.commandWorked(primary.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
currentCheckNumber = assertHasConnPoolStats(
    primary, [memberHost], {checkStatsFunc: checkHostHasNoOpenConnections}, currentCheckNumber);

// Need to re-add removed node, or the test complains about the replset config
cfg.members.push(removedMember[0]);
cfg.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

// Make sure all nodes, including the once-removed node, have the final config.
rst.waitForConfigReplication(primary);

rst.stopSet();
