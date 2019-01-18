/**
 * verify dropConnections command works for replica sets
 * @tags: [requires_replication]
 */

(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 3});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    rst.awaitSecondaryNodes();
    const cfg = primary.getDB('local').system.replset.findOne();
    const memberHost = cfg.members[2].host;

    function assertConnPoolStats(expectedAvailable, numTries) {
        var available, res;
        for (var i = 0; i < numTries; i++) {
            res = assert.commandWorked(primary.adminCommand({connPoolStats: 1}));
            available = res.hosts[memberHost].available;
            if (available == expectedAvailable) {
                return;
            }

            sleep(100);
        }

        // Fail informatively
        jsTestLog("pool stats: " + tojson(res));
        assert.eq(
            expectedAvailable,
            available,
            `wrong number of available connections to ${memberHost} after checking ${numTries} times`);
    }

    // In initial steady state, the primary should have 1 connection to the secondary
    assertConnPoolStats(1, 10);

    // To test the dropConnections command, first remove the secondary. This should have no effect
    // on the existing connection pool, but it'll prevent the primary from reconnecting to it after
    // dropConnections. Then, execute dropConnections and check that the primary has 0 connections
    // to the secondary.
    const removedMember = cfg.members.splice(2, 1);
    cfg.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

    // Reconfig did not affect the connection pool
    assertConnPoolStats(1, 10);

    // Test dropConnections
    assert.commandWorked(primary.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
    assertConnPoolStats(0, 1);

    // Need to re-add removed node, or the test complains about the replset config
    cfg.members.push(removedMember[0]);
    cfg.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

    rst.stopSet();
})();
