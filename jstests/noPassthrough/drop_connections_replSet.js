/**
 * verify dropConnections command works for replica sets
 * @tags: [
 *   requires_replication,
 * ]
 */

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
rst.awaitSecondaryNodes();

function getConnPoolHosts() {
    const ret = primary.adminCommand({connPoolStats: 1});
    assert.commandWorked(ret);
    jsTestLog("Connection pool stats by host: " + tojson(ret.hosts));
    return ret.hosts;
}

// To test the dropConnections command, first remove the secondary. This should have no effect
// on the existing connection pool, but it'll prevent the primary from reconnecting to it after
// dropConnections. Then, execute dropConnections and check that the primary has 0 connections
// to the secondary.
const cfg = primary.getDB('local').system.replset.findOne();
const memberHost = cfg.members[2].host;
assert.eq(memberHost in getConnPoolHosts(), true);

const removedMember = cfg.members.splice(2, 1);
assert.eq(removedMember[0].host, memberHost);
cfg.version++;

jsTestLog("Reconfiguring to omit " + memberHost);
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

// Reconfig did not affect the connection pool
assert.eq(memberHost in getConnPoolHosts(), true);

// Test dropConnections
jsTestLog("Dropping connections to " + memberHost);
assert.commandWorked(primary.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
assert.soon(() => {
    return !(memberHost in getConnPoolHosts());
});

// Need to re-add removed node, or the test complains about the replset config
cfg.members.push(removedMember[0]);
cfg.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

// Make sure all nodes, including the once-removed node, have the final config.
rst.waitForConfigReplication(primary);

rst.stopSet();
})();
