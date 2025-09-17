/**
 * @tags: [
 *   # This test is incompatible with 'config shard' as it creates a cluster with 0 shards in order
 *   # to be able to add shard with data on it (which is only allowed on the first shard).
 *   config_shard_incompatible,
 *   # This restriction was introduced in binary v8.3.
 *   requires_fcv_83,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("addShard with lastLTS shard and lastContinuous cluster", function () {
    if (lastLTSFCV == lastContinuousFCV) {
        jsTest.log.info("Skipping test because lastLTSFCV == lastContinuousFCV.");
        return;
    }

    beforeEach(() => {
        // Create an empty sharded cluster on lastContinuous FCV
        this.st = new ShardingTest({name: jsTestName(), shards: 0, config: 1, useHostname: false});
        assert.commandWorked(
            this.st.s.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}),
        );

        // Create a shard on lastLTS FCV
        this.newShardRs = new ReplSetTest({name: jsTestName() + "-1", host: "localhost", nodes: 1});
        this.newShardRs.startSet({shardsvr: ""});
        this.newShardRs.initiate();
        assert.commandWorked(
            this.newShardRs.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        // Add a database; for the first shard we allow data on the replica set.
        assert.commandWorked(this.newShardRs.getPrimary().getDB("testDB").xyzzy.insertOne({foo: "bar"}));
    });

    afterEach(() => {
        this.st.stop();
        this.newShardRs.stopSet();
    });

    it("should fail to add shard if not empty", () => {
        // Attempt to join the shard to the cluster. This fails since it requires a lastLTS to
        // lastContinuous FCV upgrade, which is not permitted for the first shard if it's not empty.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({addShard: this.newShardRs.getURL()}),
            ErrorCodes.IllegalOperation,
        );
        checkFCV(this.st.configRS.getPrimary().getDB("admin"), lastContinuousFCV);
        checkFCV(this.newShardRs.getPrimary().getDB("admin"), lastLTSFCV);

        const coll = this.newShardRs.getPrimary().getDB("testDB").xyzzy;
        assert.sameMembers(coll.find({}, {_id: 0}).toArray(), [{foo: "bar"}]);
    });

    it("after failing, the shard can be added if the sharded cluster is upgraded to latest", () => {
        assert.commandFailedWithCode(
            this.st.s.adminCommand({addShard: this.newShardRs.getURL()}),
            ErrorCodes.IllegalOperation,
        );

        assert.commandWorked(this.st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        checkFCV(this.st.configRS.getPrimary().getDB("admin"), latestFCV);
        checkFCV(this.newShardRs.getPrimary().getDB("admin"), lastLTSFCV);

        assert.commandWorked(this.st.s.adminCommand({addShard: this.newShardRs.getURL()}));
        checkFCV(this.st.configRS.getPrimary().getDB("admin"), latestFCV);
        checkFCV(this.newShardRs.getPrimary().getDB("admin"), latestFCV);

        const coll = this.st.s.getDB("testDB").xyzzy;
        assert.sameMembers(coll.find({}, {_id: 0}).toArray(), [{foo: "bar"}]);
    });
});
