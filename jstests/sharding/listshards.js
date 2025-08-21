/**
 * Test the listShards command by adding stand-alone and replica-set shards to a cluster
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("listShards correct functionality test", function () {
    before(() => {
        // TODO SERVER-50144 Remove this and allow orphan checking.
        // This test calls removeShard which can leave docs in config.rangeDeletions in state
        // "pending", therefore preventing orphans from being cleaned up.
        TestData.skipCheckOrphans = true;
        this.isMultiversion =
            Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);

        this.checkShardName = function (shardName, shardsArray) {
            var found = false;
            shardsArray.forEach((shardObj) => {
                if (shardObj._id === shardName) {
                    found = true;
                    return;
                }
            });
            return found;
        };

        this.st = new ShardingTest({name: "listShardsTest", shards: 1, mongos: 1, other: {useHostname: true}});

        // add replica set named 'repl1'
        this.rs1 = new ReplSetTest({name: "repl1", nodes: 1, useHostName: true, nodeOptions: {shardsvr: ""}});

        // add replica set named 'repl2'
        this.rs2 = new ReplSetTest({name: "repl2", nodes: 1, useHostName: true, nodeOptions: {shardsvr: ""}});

        this.mongos = this.st.s0;
    });

    beforeEach(() => {
        this.rs1.startSet();
        this.rs1.initiate();

        this.rs2.startSet();
        this.rs2.initiate();

        // Add the replica set to the cluster
        assert.commandWorked(this.st.admin.runCommand({addShard: this.rs1.getURL()}));
    });

    afterEach(() => {
        removeShard(this.st, "repl1");
        removeShard(this.st, "repl2");

        /// Stop the replica sets
        this.rs1.stopSet();
        this.rs2.stopSet();

        // Check that the number of shards remains the same after each test case
        assert.eq(1, this.st.s.getDB("config").shards.count());
    });

    after(() => {
        this.st.stop();
    });

    it("listShards returns all shards", () => {
        const res = this.mongos.adminCommand("listShards");
        assert.commandWorked(res, "listShards command failed");
        const shardsArray = res.shards;
        assert.eq(shardsArray.length, 2);
    });

    it("listShards returns correct shards after adding new shard", () => {
        // Add the replica set to the cluster
        let res = this.st.admin.runCommand({addShard: this.rs2.getURL()});
        assert.commandWorked(res, "addShard command failed");
        res = this.mongos.adminCommand("listShards");
        assert.commandWorked(res, "listShards command failed");
        const shardsArray = res.shards;
        assert.eq(shardsArray.length, 3);
        assert(
            this.checkShardName("repl2", shardsArray),
            "listShards command didn't return replica set shard: " + tojson(shardsArray),
        );
    });

    it("listShards returns correct shards after removing a shard", () => {
        // remove 'repl1' shard
        removeShard(this.st, "repl1");
        const res = this.mongos.adminCommand("listShards");
        assert.commandWorked(res, "listShards command failed");
        const shardsArray = res.shards;
        assert.eq(shardsArray.length, 1);
        assert(
            !this.checkShardName("repl1", shardsArray),
            "listShards command returned removed replica set shard: " + tojson(shardsArray),
        );
    });

    it("listShards 'draining : true' filter returns only the actively draining shards", () => {
        if (this.isMultiversion) {
            return;
        }
        assert.commandWorked(this.st.admin.runCommand({startShardDraining: "repl1"}));
        // Check that filter only returns draining shards
        const draining = this.st.admin.runCommand({listShards: 1, filter: {draining: true}});
        assert.commandWorked(draining, "listShards command failed");
        const shardsArray = draining.shards;
        assert.eq(shardsArray.length, 1);
        assert(
            this.checkShardName("repl1", shardsArray),
            "listShards command didn't return the draining shard: " + tojson(shardsArray),
        );
    });

    it("listShards 'draining : false' filter returns only the non-draining shards", () => {
        if (this.isMultiversion) {
            return;
        }
        assert.commandWorked(this.st.admin.runCommand({startShardDraining: "repl1"}));
        const non_draining = this.st.admin.runCommand({listShards: 1, filter: {draining: false}});
        assert.commandWorked(non_draining);
        const shardsArray = non_draining.shards;
        assert.eq(shardsArray.length, 1);
        assert(
            this.checkShardName(this.st.shard0.shardName, shardsArray),
            "listShards command didn't return the non-draining shard: " + tojson(shardsArray),
        );
    });

    it("listShards wrong draining filter value throws error", () => {
        if (this.isMultiversion) {
            return;
        }
        assert.commandFailedWithCode(
            this.st.admin.runCommand({listShards: 1, filter: {draining: 1}}),
            ErrorCodes.TypeMismatch,
        );
    });

    it("listShards unknown filter throws error", () => {
        if (this.isMultiversion) {
            return;
        }
        // Call listShards with unknown filter
        assert.commandFailedWithCode(
            this.st.admin.runCommand({listShards: 1, filter: {droining: true}}),
            ErrorCodes.IDLUnknownField,
        );
    });

    it("listShards returns correct shards after stopping the draining", () => {
        if (this.isMultiversion) {
            return;
        }
        assert.commandWorked(this.st.admin.runCommand({startShardDraining: "repl1"}));
        // Stop draining the 'repl1' shard
        const stop_draining = this.st.admin.runCommand({stopShardDraining: "repl1"});
        assert.commandWorked(stop_draining);
        // Check that filter doesn't return the 'repl1' shard
        const res = this.st.admin.runCommand({listShards: 1, filter: {draining: true}});
        assert.commandWorked(res, "listShards command failed");
        const shardsArray = res.shards;
        assert.eq(shardsArray.length, 0);
    });

    it("listShards with draining filter and unknown filters throws error", () => {
        if (this.isMultiversion) {
            return;
        }
        assert.commandFailedWithCode(
            this.st.admin.runCommand({listShards: 1, filter: {draining: true, droining: true}}),
            ErrorCodes.IDLUnknownField,
        );
    });
});
