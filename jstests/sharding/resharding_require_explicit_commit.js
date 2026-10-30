/**
 * Tests that when reshardingRequireExplicitCommit is enabled, the resharding coordinator does not
 * engage the critical section on its own even after all recipients are within the commit
 * threshold, and that the operation commits once the user runs commitReshardCollection.
 *
 * @tags: [
 *   uses_atclustertime,
 *   # Requires the server parameter added with this test.
 *   requires_fcv_80,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const kThresholdMillis = 10 * 1000;
const kMaxDelayBetweenQueriesMillis = 1000;

const dbName = "reshardingDb";
const collName = "coll";
const ns = dbName + "." + collName;

function runReshardCollection(host, ns, recipientShardName0, recipientShardName1) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        _presetReshardedChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, recipientShardId: recipientShardName0},
            {min: {newKey: 0}, max: {newKey: MaxKey}, recipientShardId: recipientShardName1},
        ],
    });
}

describe("reshardingRequireExplicitCommit", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            other: {
                configOptions: {
                    setParameter: {
                        reshardingRequireExplicitCommit: true,
                        reshardingMinimumOperationDurationMillis: 0,
                        reshardingDelayBeforeRemainingOperationTimeQueryMillis: 0,
                        reshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis:
                            kMaxDelayBetweenQueriesMillis,
                        remainingReshardingOperationTimeThresholdMillis: kThresholdMillis,
                    },
                },
            },
        });
        const st = this.st;
        this.configPrimary = st.configRS.getPrimary();

        const coll = st.s.getCollection(ns);
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {oldKey: 1}, [
            {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: st.shard0.shardName},
            {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: st.shard1.shardName},
        ]);
        assert.commandWorked(
            coll.insert([
                {_id: -1, oldKey: -10, newKey: 10},
                {_id: 1, oldKey: 10, newKey: -10},
            ]),
        );

        // Any attempt by the coordinator to engage the critical section passes through this fail
        // point, so its hit count tells us whether the coordinator tried to commit on its own.
        this.blockingWritesFp = configureFailPoint(
            this.configPrimary,
            "reshardingPauseCoordinatorBeforeBlockingWrites",
        );

        this.reshardThread = new Thread(
            runReshardCollection,
            st.s.host,
            ns,
            st.shard0.shardName,
            st.shard1.shardName,
        );
        this.reshardThread.start();
    });

    after(function () {
        this.st.stop();
    });

    function getCoordinatorDoc(st) {
        return st.s.getCollection("config.reshardingOperations").findOne({ns: ns});
    }

    function getCoordinatorCurrentOp(conn) {
        const inprog = conn
            .getDB("admin")
            .currentOp({ns: ns, desc: {$regex: "ReshardingMetricsCoordinatorService"}}).inprog;
        assert.lte(inprog.length, 1, "Found more than one resharding coordinator", {inprog});
        return inprog[0];
    }

    function getTimesEntered(conn, failPointName) {
        // Re-enabling an already enabled fail point leaves it on and reports its hit count.
        return assert.commandWorked(
            conn.adminCommand({configureFailPoint: failPointName, mode: "alwaysOn"}),
        ).count;
    }

    it("does not engage the critical section once recipients are within the threshold", function () {
        const st = this.st;
        const configPrimary = this.configPrimary;

        jsTest.log.info("Waiting for all recipients to be within the commit threshold");
        assert.soon(() => {
            const op = getCoordinatorCurrentOp(configPrimary);
            const remainingSecs = op?.allShardsHighestRemainingOperationTimeEstimatedSecs;
            // The estimate is truncated to seconds and is -1 when unknown.
            return (
                op?.coordinatorState === "applying" &&
                remainingSecs !== undefined &&
                remainingSecs >= 0 &&
                remainingSecs < kThresholdMillis / 1000
            );
        });

        // The commit monitor updates the remaining time estimate in the same step where it decides
        // whether to engage the critical section. So once the monitor starts another round after
        // the estimate above was reported, it has already declined to engage the critical section
        // for recipients that were within the threshold.
        jsTest.log.info("Waiting for the commit monitor to start another round");
        const monitorRoundFp = configureFailPoint(configPrimary, "hangBeforeQueryingRecipients");
        monitorRoundFp.wait();
        monitorRoundFp.off();

        assert.eq(
            this.blockingWritesFp.timesEntered,
            getTimesEntered(configPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites"),
            "Coordinator tried to engage the critical section without commitReshardCollection",
        );
        assert.eq("applying", getCoordinatorDoc(st).state);
        assert.eq({oldKey: 1}, st.s.getCollection("config.collections").findOne({_id: ns}).key);
    });

    it("commits after commitReshardCollection", function () {
        const st = this.st;

        this.blockingWritesFp.off();
        assert.commandWorked(st.s.adminCommand({commitReshardCollection: ns}));
        assert.commandWorked(this.reshardThread.returnData());

        assert.eq({newKey: 1}, st.s.getCollection("config.collections").findOne({_id: ns}).key);
        assert.eq(null, getCoordinatorDoc(st));
    });
});
