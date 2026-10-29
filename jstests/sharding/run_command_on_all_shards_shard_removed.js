/** Test for FixtureHelpers behavior when a shard is removed during command execution. */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("runCommandOnAllShards with a shard that goes away", function () {
    before(async function () {
        this.originalShardsAddedRemoved = TestData.shardsAddedRemoved;
        TestData.shardsAddedRemoved = true;

        this.st = new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}});
        this.adminDb = this.st.s.getDB("admin");

        this.extraShardName = "shard-rs1";
        this.extraShard = new ReplSetTest({nodes: 1, nodeOptions: {shardsvr: ""}});
        this.extraShard.startSet();
        this.extraShard.initiate();
        assert.commandWorked(
            this.adminDb.runCommand({
                addShard: this.extraShard.getURL(),
                name: this.extraShardName,
            }),
        );
        this.extraShardStopped = false;

        await import(
            "jstests/libs/override_methods/implicitly_retry_on_shard_transition_errors.js"
        );
    });

    after(function () {
        TestData.shardsAddedRemoved = this.originalShardsAddedRemoved;

        this.st.stop();
        if (!this.extraShardStopped) {
            this.extraShard.stopSet();
        }
    });

    it("still fails when the command fails on a live shard", function () {
        assert.throws(() =>
            FixtureHelpers.runCommandOnAllShards({
                db: this.adminDb,
                cmdObj: {configureFailPoint: "thisFailPointDoesNotExist", mode: "off"},
            }),
        );
    });

    it("skips a shard that was removed and shut down while the command was being sent", function () {
        const cmdObj = {configureFailPoint: "suspendRangeDeletion", mode: "off"};

        let visited = 0;
        const callsPerHost = {};
        const responses = FixtureHelpers.mapOnEachShardNode({
            db: this.adminDb,
            func: (shardDb) => {
                const host = shardDb.getMongo().host;
                callsPerHost[host] = (callsPerHost[host] ?? 0) + 1;
                if (visited++ === 0) {
                    removeShard(this.st, this.extraShardName);
                    this.extraShardStopped = true;
                    this.extraShard.stopSet();
                }
                assert.commandWorked(shardDb.runCommand(cmdObj));
                return callsPerHost[host];
            },
        });

        assert.eq(
            1,
            this.st.s.getDB("config").shards.count(),
            "the second shard should have been removed",
        );
        assert.eq(
            [1],
            responses,
            "the callback should not run again on the shard that already completed",
            {callsPerHost},
        );
    });
});
