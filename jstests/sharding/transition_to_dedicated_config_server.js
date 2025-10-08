/**
 * Test transition to dedicated config server from sharded cluster with embedded config server works
 * correctly.
 * @tags: [
 * requires_fcv_83
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Check transition to dedicated config server starts, returns correct status, stops, and commits properly", function () {
    before(() => {
        jsTest.log.info("Create sharded cluster 'st' with embedded config server");
        this.st = new ShardingTest({
            name: "st",
            shards: 2,
            other: {
                enableBalancer: true,
                configShard: true,
                rsOptions: {setParameter: {orphanCleanupDelaySecs: 0}},
            },
        });
    });

    beforeEach(() => {
        // Add sharded collection
        assert.commandWorked(this.st.s.adminCommand({enableSharding: "testDB", primaryShard: "config"}));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: "testDB.testColl", key: {_id: 1}}));
        this.st.s.getDB("testDB").testColl.insert({_id: 1});
        this.st.s.getDB("testDB").testColl.insert({_id: 2});
        // Add unsharded collection
        assert.commandWorked(this.st.s.getDB("testDB").testCollUnsharded.insert({_id: 1}));
    });

    afterEach(() => {
        const numShards = this.st.s.getDB("config").shards.count();
        if (numShards == 2) {
            jsTest.log.info("Stop transition to dedicated config server");
            assert.commandWorked(this.st.s.adminCommand({stopTransitionToDedicatedConfigServer: 1}));
            // Check that config shard draining has stopped successfully
            const notDrainingShards = this.st.s.getDB("config").shards.find({"draining": true}).toArray();
            assert.eq(0, notDrainingShards.length);
        } else {
            jsTest.log.info("Transition back to embedded config server");
            assert.commandWorked(this.st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
        }

        // Clean data
        assert.commandWorked(this.st.s.getDB("testDB").dropDatabase());
    });

    after(() => {
        jsTest.log.info("Stop sharded cluster 'st'");
        this.st.stop();
    });

    it("Test startTransitionToDedicatedConfigServer command", function () {
        assert.commandWorked(this.st.s.adminCommand({startTransitionToDedicatedConfigServer: 1}));
        // Check that the config shard is marked as draining
        const drainingShards = this.st.s.getDB("config").shards.find({"draining": true}).toArray();
        assert.eq(1, drainingShards.length);
        assert.eq("config", drainingShards[0]._id);
    });

    it("Test getTransitionToDedicatedConfigServerStatus command", function () {
        // Start shard draining
        jsTest.log.info("Start transition to dedicated config server");
        assert.commandWorked(this.st.s.adminCommand({startTransitionToDedicatedConfigServer: 1}));
        this.st.configRS.awaitReplication();

        // Check draining status is ongoing
        const drainingStatus0 = this.st.s.adminCommand({getTransitionToDedicatedConfigServerStatus: 1});
        assert.commandWorked(drainingStatus0);
        assert.eq("ongoing", drainingStatus0.state);

        jsTest.log.info("Move unsharded collection's primary to another shard");
        assert.commandWorked(this.st.s.adminCommand({movePrimary: "testDB", to: this.st.shard1.shardName}));

        jsTest.log.info("Wait for draining to complete");
        assert.soon(() => {
            const drainingStatus1 = this.st.s.adminCommand({getTransitionToDedicatedConfigServerStatus: 1});
            assert.commandWorked(drainingStatus1);
            return "drainingComplete" == drainingStatus1.state;
        }, "getTransitionToDedicatedConfigServerStatus did not return 'drainingComplete' status within the timeout");
    });

    it("Test commitTransitionToDedicatedConfigServer command", function () {
        jsTest.log.info("Start transition to dedicated config server");
        assert.commandWorked(this.st.s.adminCommand({startTransitionToDedicatedConfigServer: 1}));
        this.st.configRS.awaitReplication();

        jsTest.log.info("Move unsharded collection's primary to another shard");
        assert.commandWorked(this.st.s.adminCommand({movePrimary: "testDB", to: this.st.shard1.shardName}));

        jsTest.log.info("Wait for draining to complete");
        assert.soon(() => {
            const drainingStatus = this.st.s.adminCommand({getTransitionToDedicatedConfigServerStatus: 1});
            assert.commandWorked(drainingStatus);
            return "drainingComplete" == drainingStatus.state;
        }, "getTransitionToDedicatedConfigServerStatus did not return 'drainingComplete' status within the timeout");

        jsTest.log.info("Commit transition to dedicated config server");
        assert.commandWorked(this.st.s.adminCommand({commitTransitionToDedicatedConfigServer: 1}));
    });

    it("Test startShardDraining command cannot be run on config shard", function () {
        assert.commandFailedWithCode(
            this.st.s.adminCommand({startShardDraining: "config"}),
            ErrorCodes.IllegalOperation,
        );
    });

    it("Test shardDrainingStatus command cannot be run on config shard", function () {
        assert.commandFailedWithCode(
            this.st.s.adminCommand({shardDrainingStatus: "config"}),
            ErrorCodes.IllegalOperation,
        );
    });

    it("Test stopShardDraining command cannot be run on config shard", function () {
        assert.commandFailedWithCode(
            this.st.s.adminCommand({stopShardDraining: "config"}),
            ErrorCodes.IllegalOperation,
        );
    });

    it("Test commitShardRemoval command cannot be run on config shard", function () {
        assert.commandFailedWithCode(
            this.st.s.adminCommand({commitShardRemoval: "config"}),
            ErrorCodes.IllegalOperation,
        );
    });
});
