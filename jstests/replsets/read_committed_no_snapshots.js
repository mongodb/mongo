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
    var replTest = new ReplSetTest({
        name: name,
        nodes: [
            {},
            {rsConfig: {priority: 0}},
            {
              setParameter: {"failpoint.disableSnapshotting": "{'mode':'alwaysOn'}"},
              rsConfig: {priority: 0}
            }
        ],
        nodeOptions: {enableMajorityReadConcern: ''},
        settings: {protocolVersion: 1}
    });

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        replTest.stopSet();
        return;
    }

    // Cannot wait for a stable checkpoint due to the no-snapshot secondary.
    replTest.initiateWithAnyNodeAsPrimary(
        null, "replSetInitiate", {doNotWaitForStableCheckpoint: true});

    // Get connections and collection.
    var primary = replTest.getPrimary();
    var healthySecondary = replTest._slaves[0];
    healthySecondary.setSlaveOk();
    var noSnapshotSecondary = replTest._slaves[1];
    noSnapshotSecondary.setSlaveOk();

    // Do a write, wait for it to replicate, and ensure it is visible.
    var res = primary.getDB(name).runCommandWithMetadata(  //
        {
          insert: "foo",
          documents: [{_id: 1, state: 0}],
          writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS}
        },
        {"$replData": 1});
    assert.commandWorked(res.commandReply);

    // We need to propagate the lastOpVisible from the primary as afterOpTime in the secondaries to
    // ensure we wait for the write to be in the majority committed view.
    var lastOp = res.metadata["$replData"].lastOpVisible;

    // Timeout is based on heartbeat timeout.
    assert.commandWorked(healthySecondary.getDB(name).foo.runCommand(
        'find',
        {"readConcern": {"level": "majority", "afterOpTime": lastOp}, "maxTimeMS": 10 * 1000}));

    // Ensure maxTimeMS times out while waiting for this snapshot
    assert.commandFailedWithCode(
        noSnapshotSecondary.getDB(name).foo.runCommand(
            'find', {"readConcern": {"level": "majority"}, "maxTimeMS": 1000}),
        ErrorCodes.ExceededTimeLimit);

    // Reconfig to make the no-snapshot secondary the primary
    var config = primary.getDB("local").system.replset.findOne();
    config.members[0].priority = 0;
    config.members[2].priority = 1;
    config.version++;
    primary = reconfig(replTest, config, true);

    // Ensure maxTimeMS times out while waiting for this snapshot
    assert.commandFailedWithCode(
        primary.getSiblingDB(name).foo.runCommand(
            'find', {"readConcern": {"level": "majority"}, "maxTimeMS": 1000}),
        ErrorCodes.ExceededTimeLimit);
    replTest.stopSet();
})();
