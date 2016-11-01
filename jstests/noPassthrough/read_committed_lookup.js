/**
 * Tests that a $lookup and $graphLookup stage within an aggregation pipeline will read only
 * committed data if the pipeline is using a majority readConcern. This is tested both on a replica
 * set, and on a sharded cluster with one shard which is a replica set.
 */

load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

(function() {
    "use strict";

    /**
     * Test readCommitted lookup/graphLookup. 'db' must be the test database for either the shard
     * primary or mongos instance. 'secondary' is the shard/replica set secondary. If 'db' is backed
     * by a mongos instance then the associated cluster should have only a single shard.
     */
    function testReadCommittedLookup(db, secondary) {
        /**
         * Uses the 'rsSyncApplyStop' fail point to stop application of oplog entries on the given
         * secondary.
         */
        function pauseReplication(sec) {
            assert.commandWorked(
                sec.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}),
                "failed to enable fail point on secondary");
        }

        /**
         * Turns off the 'rsSyncApplyStop' fail point to resume application of oplog entries on the
         * given secondary.
         */
        function resumeReplication(sec) {
            assert.commandWorked(
                sec.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}),
                "failed to disable fail point on secondary");
        }

        const aggCmdLookupObj = {
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

        const aggCmdGraphLookupObj = {
            aggregate: "local",
            pipeline: [{
                $graphLookup: {
                    from: "foreign",
                    startWith: '$foreignKey',
                    connectFromField: 'foreignKey',
                    connectToField: "matchedField",
                    as: "match"
                }
            }],
            readConcern: {
                level: "majority",
            }
        };

        // Seed matching data.
        const majorityWriteConcernObj = {writeConcern: {w: "majority", wtimeout: 60 * 1000}};
        db.local.deleteMany({}, majorityWriteConcernObj);
        const localId = db.local.insertOne({foreignKey: "x"}, majorityWriteConcernObj).insertedId;
        db.foreign.deleteMany({}, majorityWriteConcernObj);
        const foreignId =
            db.foreign.insertOne({matchedField: "x"}, majorityWriteConcernObj).insertedId;

        const expectedMatchedResult = [{
            _id: localId,
            foreignKey: "x",
            match: [
                {_id: foreignId, matchedField: "x"},
            ],
        }];
        const expectedUnmatchedResult = [{
            _id: localId,
            foreignKey: "x",
            match: [],
        }];

        // Confirm lookup/graphLookup return the matched result.
        let result = db.runCommand(aggCmdLookupObj).result;
        assert.eq(result, expectedMatchedResult);

        result = db.runCommand(aggCmdGraphLookupObj).result;
        assert.eq(result, expectedMatchedResult);

        // Stop oplog application on the secondary so that it won't acknowledge updates.
        pauseReplication(secondary);

        // Update foreign data to no longer match, without a majority write concern.
        db.foreign.updateOne({_id: foreignId}, {$set: {matchedField: "non-match"}});

        // lookup/graphLookup should not see the update, since it has not been acknowledged by the
        // secondary.
        result = db.runCommand(aggCmdLookupObj).result;
        assert.eq(result, expectedMatchedResult);

        result = db.runCommand(aggCmdGraphLookupObj).result;
        assert.eq(result, expectedMatchedResult);

        // Restart oplog application on the secondary and wait for it's snapshot to catch up.
        resumeReplication(secondary);
        rst.awaitLastOpCommitted();

        // Now lookup/graphLookup should report that the documents don't match.
        result = db.runCommand(aggCmdLookupObj).result;
        assert.eq(result, expectedUnmatchedResult);

        result = db.runCommand(aggCmdGraphLookupObj).result;
        assert.eq(result, expectedUnmatchedResult);
    }

    //
    // Confirm majority readConcern works on a replica set.
    //
    const replSetName = "lookup_read_majority";
    let rst = new ReplSetTest({
        nodes: 3,
        name: replSetName,
        nodeOptions: {
            enableMajorityReadConcern: "",
            shardsvr: "",
        }
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    const nodes = rst.nodeList();
    const config = {
        _id: replSetName,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1], priority: 0},
            {_id: 2, host: nodes[2], arbiterOnly: true},
        ]
    };
    updateConfigIfNotDurable(config);
    rst.initiate(config);

    let shardSecondary = rst.liveNodes.slaves[0];

    testReadCommittedLookup(rst.getPrimary().getDB("test"), shardSecondary);

    //
    // Confirm read committed works on a cluster with a database that is not sharding enabled.
    //
    let st = new ShardingTest({
        manualAddShard: true,
    });
    assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));
    testReadCommittedLookup(st.s.getDB("test"), shardSecondary);

    //
    // Confirm read committed works on a cluster with:
    // - A sharding enabled database
    // - An unsharded local collection
    //
    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    testReadCommittedLookup(st.s.getDB("test"), shardSecondary);

    //
    // Confirm read committed works on a cluster with:
    // - A sharding enabled database
    // - A sharded local collection.
    //
    assert.commandWorked(st.s.getDB("test").runCommand(
        {createIndexes: 'local', indexes: [{name: "foreignKey_1", key: {foreignKey: 1}}]}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.local', key: {foreignKey: 1}}));
    testReadCommittedLookup(st.s.getDB("test"), shardSecondary);

    st.stop();
})();
