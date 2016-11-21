/**
 * Test basic read committed maxTimeMS timeout while waiting for a committed snapshot:
 *  - Reads with an 'afterOpTime' snapshot >= current time should be able to see things that
 *    happened before or at that opTime.
 *  - Reads should time out if there are no snapshots available on secondary.
 */

load("jstests/replsets/rslib.js");  // For reconfig and startSetIfSupportsReadMajority.

(function() {
    "use strict";

    // Set up a set and grab things for later.
    var name = "read_committed_no_snapshots";
    var replTest =
        new ReplSetTest({name: name, nodes: 3, nodeOptions: {enableMajorityReadConcern: ''}});

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    var nodes = replTest.nodeList();
    var config = {
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ],
        "protocolVersion": 1
    };

    replTest.initiate(config);

    // Get connections and collection.
    var primary = replTest.getPrimary();
    var secondary = replTest.liveNodes.slaves[0];
    var secondaryId = replTest.getNodeId(secondary);
    var db = primary.getDB(name);

    // Do a write, wait for it to replicate, and ensure it is visible.
    var res = db.runCommandWithMetadata("insert",
                                        {
                                          insert: "foo",
                                          documents: [{_id: 1, state: 0}],
                                          writeConcern: {w: "majority", wtimeout: 60 * 1000}
                                        },
                                        {"$replData": 1});
    assert.commandWorked(res.commandReply);

    // We need to propagate the lastOpVisible from the primary as afterOpTime in the secondary to
    // ensure
    // we wait for the write to be in the majority committed view.
    var lastOp = res.metadata["$replData"].lastOpVisible;

    secondary.setSlaveOk();
    // Timeout is based on heartbeat timeout.
    assert.commandWorked(secondary.getDB(name).foo.runCommand(
        'find',
        {"readConcern": {"level": "majority", "afterOpTime": lastOp}, "maxTimeMS": 10 * 1000}));

    // Disable snapshotting via failpoint
    secondary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'});

    // Resync to drop any existing snapshots
    secondary.adminCommand({resync: 1});

    // Ensure maxTimeMS times out while waiting for this snapshot
    assert.commandFailed(secondary.getDB(name).foo.runCommand(
        'find', {"readConcern": {"level": "majority"}, "maxTimeMS": 1000}));

    // Reconfig to make the secondary the primary
    var config = primary.getDB("local").system.replset.findOne();
    config.members[0].priority = 0;
    config.members[1].priority = 3;
    config.version++;
    primary = reconfig(replTest, config, true);

    // Ensure maxTimeMS times out while waiting for this snapshot
    assert.commandFailed(primary.getSiblingDB(name).foo.runCommand(
        'find', {"readConcern": {"level": "majority"}, "maxTimeMS": 1000}));
})();
