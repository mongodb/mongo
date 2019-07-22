/**
 * Tests that the server logs ops on the secondary if and only if they are slow to apply.
 * We should only report ops if they take longer than "slowMS" to apply on a secondary.
 * We intentionally target CRUD ops in this test, since we know we should be the only ones
 * issuing them. See below for details on how we simulate quickness and slowness.
 */

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    let name = "log_secondary_oplog_application";
    let rst = ReplSetTest({name: name, nodes: 2});
    rst.startSet();

    let nodes = rst.nodeList();
    rst.initiate({
        "_id": name,
        "members": [{"_id": 0, "host": nodes[0]}, {"_id": 1, "host": nodes[1], "priority": 0}]
    });

    let primary = rst.getPrimary();
    let secondary = rst.getSecondary();

    /**
     * Part 1: Issue a fast op and make sure that we do *not* log it.
     * We ensure the op is always considered fast by vastly increasing the "slowMS" threshold.
     */

    // Create collection explicitly so the insert doesn't have to do it.
    assert.commandWorked(primary.getDB(name).createCollection("fastOp"));
    rst.awaitReplication();

    // Set "slowMS" to a very high value (in milliseconds).
    assert.commandWorked(secondary.getDB(name).setProfilingLevel(1, 60 * 60 * 1000));

    // Issue a write and make sure we replicate it.
    assert.commandWorked(primary.getDB(name)["fastOp"].insert({"fast": "cheetah"}));
    rst.awaitReplication();

    // The op should not have been logged.
    assert.throws(function() {
        checkLog.contains(secondary, "cheetah", 1 * 1000);
    });

    /**
     * Part 2: Issue a slow op and make sure that we *do* log it.
     * We use a failpoint in SyncTail::syncApply which blocks after we read the time at the start
     * of the application of the op, and we wait there to simulate slowness.
     */

    // Create collection explicitly so the insert doesn't have to do it.
    assert.commandWorked(primary.getDB(name).createCollection("slowOp"));
    rst.awaitReplication();

    // Set "slowMS" to a low value (in milliseconds).
    assert.commandWorked(secondary.getDB(name).setProfilingLevel(1, 20));

    // Hang right after taking note of the start time of the application.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: "hangAfterRecordingOpApplicationStartTime", mode: "alwaysOn"}));

    // Issue a write and make sure we've hit the failpoint before moving on.
    assert.commandWorked(primary.getDB(name)["slowOp"].insert({"slow": "sloth"}));
    checkLog.contains(secondary,
                      "syncApply - fail point hangAfterRecordingOpApplicationStartTime enabled");

    // Wait for an amount of time safely above the "slowMS" we set.
    sleep(0.5 * 1000);

    // Disable the failpoint so the op finish can applying.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: "hangAfterRecordingOpApplicationStartTime", mode: "off"}));

    // Make sure we log that insert op.
    rst.awaitReplication();
    checkLog.contains(secondary, "sloth");

    rst.stopSet();

})();