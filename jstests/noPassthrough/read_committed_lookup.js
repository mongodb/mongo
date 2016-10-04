/**
 * Tests that a $lookup stage within an aggregation pipeline will read only committed data if the
 * pipeline is using a majority readConcern. This is tested both on a replica set, and on a sharded
 * cluster with one shard which is a replica set.
 */

load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

(function() {
    "use strict";

    //
    // First do everything with a replica set.
    //
    var replSetName = "lookup_read_majority";
    var rst = new ReplSetTest({
        nodes: 3,
        name: replSetName,
        nodeOptions: {
            enableMajorityReadConcern: "",
        }
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    var nodes = rst.nodeList();
    var config = {
        _id: replSetName,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1], priority: 0},
            {_id: 2, host: nodes[2], arbiterOnly: true},
        ]
    };
    updateConfigIfNotDurable(config);
    rst.initiate(config);

    var primary = rst.getPrimary();
    var secondary = rst.liveNodes.slaves[0];
    var db = primary.getDB("test");

    /**
     * Uses the 'rsSyncApplyStop' fail point to stop application of oplog entries on the given
     * secondary.
     */
    function pauseReplication(secondary) {
        assert.commandWorked(
            secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}),
            "failed to enable fail point on secondary");
    }

    /**
     * Turns off the 'rsSyncApplyStop' fail point to resume application of oplog entries on the
     * given secondary.
     */
    function resumeReplication(secondary) {
        assert.commandWorked(
            secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}),
            "failed to disable fail point on secondary");
    }

    // Seed matching data.
    var majorityWriteConcernObj = {writeConcern: {w: "majority", wtimeout: 60 * 1000}};
    var localId = db.local.insertOne({foreignKey: "x"}, majorityWriteConcernObj).insertedId;
    var foreignId = db.foreign.insertOne({matchedField: "x"}, majorityWriteConcernObj).insertedId;

    // A $lookup stage within an aggregation pipeline using majority readConcern should see the two
    // documents matching.
    var aggCmdObj = {
        aggregate: "local",
        pipeline: [
            {
              $lookup: {
                  from: "foreign",
                  localField: "foreignKey",
                  foreignField: "matchedField",
                  as: "match",
              }
            },
        ],
        readConcern: {
            level: "majority",
        }
    };
    var expectedMatchedResult = [{
        _id: localId,
        foreignKey: "x",
        match: [
            {_id: foreignId, matchedField: "x"},
        ],
    }];
    var expectedUnmatchedResult = [{
        _id: localId,
        foreignKey: "x",
        match: [],
    }];
    var result = db.runCommand(aggCmdObj).result;
    assert.eq(result, expectedMatchedResult);

    // Stop oplog application on the secondary so that it won't acknowledge updates.
    pauseReplication(secondary);

    // Update foreign data to no longer match, without a majority write concern.
    db.foreign.updateOne({_id: foreignId}, {$set: {matchedField: "non-match"}});

    // The $lookup should not see the update, since it has not been acknowledged by the secondary.
    result = db.runCommand(aggCmdObj).result;
    assert.eq(result, expectedMatchedResult);

    // Restart oplog application on the secondary and wait for it's snapshot to catch up.
    resumeReplication(secondary);
    rst.awaitLastOpCommitted();

    // Now the $lookup stage should report that the documents don't match.
    result = db.runCommand(aggCmdObj).result;
    assert.eq(result, expectedUnmatchedResult);

    //
    // Now through a mongos.
    //
    var st = new ShardingTest({
        manualAddShard: true,
    });
    st.adminCommand({addShard: rst.getURL()});

    // Make sure the documents still match.
    result = st.s.getDB("test").runCommand(aggCmdObj).result;
    assert.eq(result, expectedUnmatchedResult);

    // Stop oplog application on the secondary again and update the looked-up document so that it
    // will match.
    pauseReplication(secondary);
    db.foreign.updateOne({_id: foreignId}, {$set: {matchedField: "x"}});

    // The update should not be visible, so the documents still shouldn't match.
    result = st.s.getDB("test").runCommand(aggCmdObj).result;
    assert.eq(result, expectedUnmatchedResult);

    // Unlock the secondary and wait for it's snapshot to catch up.
    resumeReplication(secondary);
    rst.awaitLastOpCommitted();

    // Now the documents should match again.
    result = db.runCommand(aggCmdObj).result;
    assert.eq(result, expectedMatchedResult);

    st.stop();
})();
