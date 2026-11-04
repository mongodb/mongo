/**
 * Tests that reshardingRequireExplicitCommit can be turned off at runtime. A resharding operation
 * started while the parameter is enabled stays in the applying state, and then commits on its own
 * once the parameter is disabled, without the user running commitReshardCollection.
 *
 * @tags: [
 *   uses_atclustertime,
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

describe("turning reshardingRequireExplicitCommit on and then off", function () {
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
        // point, so we can both observe it and hold the coordinator there.
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

    it("holds in the applying state while the parameter is enabled", function () {
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
            "Coordinator tried to engage the critical section while reshardingRequireExplicitCommit is enabled",
        );
        assert.eq("applying", getCoordinatorDoc(st).state);
        assert.eq({oldKey: 1}, st.s.getCollection("config.collections").findOne({_id: ns}).key);
    });

    it("commits on its own after the parameter is turned off", function () {
        const st = this.st;
        const configPrimary = this.configPrimary;

        jsTest.log.info("Turning off reshardingRequireExplicitCommit");
        assert.commandWorked(
            configPrimary.adminCommand({setParameter: 1, reshardingRequireExplicitCommit: false}),
        );

        // The commit monitor re-polls the recipients at most every
        // reshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis, so the coordinator is
        // expected to engage the critical section within that window without any user
        // intervention.
        assert(
            this.blockingWritesFp.wait({maxTimeMS: 2 * 60 * 1000}),
            "Resharding never engaged the critical section after the parameter was turned off",
        );
        this.blockingWritesFp.off();

        assert.commandWorked(this.reshardThread.returnData());

        assert.eq({newKey: 1}, st.s.getCollection("config.collections").findOne({_id: ns}).key);
        assert.eq(null, getCoordinatorDoc(st));
    });
});
