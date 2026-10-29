/**
 * Tests collMod error handling for non-retriable errors. Validates that cleanup will be triggered
 * for non-tracked, timeseries modifications and that the coordinator must always make progress for
 * tracked, timeseries modifications.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function checkCleanupComplete(configDB, dbPrimary, ns) {
    const collEntry = configDB.collections.findOne({_id: ns});
    assert(collEntry, "expected sharded timeseries collection in config.collections");
    assert.eq(
        undefined,
        collEntry.allowChunkOperations,
        "chunk operations should be unblocked after collmod cleans up",
        {collEntry},
    );
    const criticalSection = dbPrimary
        .getCollection("config.collection_critical_sections")
        .findOne({_id: ns});
    assert.eq(
        null,
        criticalSection,
        "db primary shard should release the critical section before the coordinator completes",
    );
}

describe("collMod error handling on sharded timeseries collections", function () {
    const dbName = jsTestName();
    const collName = "timeseriesColl";
    const timeField = "tm";
    const metaField = "mt";

    before(function () {
        this.st = new ShardingTest({shards: 2, rs: {nodes: 2}});
        let db = this.st.s.getDB(dbName);

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            db.createCollection(collName, {
                timeseries: {
                    timeField: timeField,
                    metaField: metaField,
                    granularity: "seconds",
                },
            }),
        );
        assert.commandWorked(
            db.adminCommand({
                shardCollection: `${dbName}.${collName}`,
                key: {[metaField]: 1},
            }),
        );
        assert.commandWorked(
            db.getCollection(getTimeseriesCollForDDLOps(db, collName)).createIndex({a: 1}),
        );

        this.targetingNs = `${dbName}.${getTimeseriesCollForDDLOps(db, collName)}`;
    });

    after(function () {
        this.st.stop();
    });

    it("Coordinator retries for tracked, timeseries modifications during kUpdateConfig", function () {
        const failPoint = configureFailPoint(
            this.st.rs0.getPrimary(),
            "throwErrorDuringConfigUpdatePhase",
            {},
            {times: 1},
        );
        assert.commandWorked(
            this.st.s
                .getDB(dbName)
                .runCommand({collMod: collName, timeseries: {granularity: "minutes"}}),
        );
        failPoint.off();

        checkCleanupComplete(this.st.s.getDB("config"), this.st.rs0.getPrimary(), this.targetingNs);
    });

    it("Coordinator retries for tracked, timeseries modifications during kUpdateShards", function () {
        const failPoint = configureFailPoint(
            this.st.rs0.getPrimary(),
            "throwErrorDuringUpdateShardsPhase",
            {},
            {times: 1},
        );
        assert.commandWorked(
            this.st.s
                .getDB(dbName)
                .runCommand({collMod: collName, timeseries: {granularity: "hours"}}),
        );
        failPoint.off();

        checkCleanupComplete(this.st.s.getDB("config"), this.st.rs0.getPrimary(), this.targetingNs);
    });

    it("Coordinator cleans up for non-tracked, timeseries modifications during kUpdateShards", function () {
        const failPoint = configureFailPoint(
            this.st.rs0.getPrimary(),
            "throwErrorDuringUpdateShardsPhase",
        );
        assert.commandFailedWithCode(
            this.st.s
                .getDB(dbName)
                .runCommand({collMod: collName, index: {keyPattern: {a: 1}, hidden: true}}),
            ErrorCodes.BadValue,
            "collMod should fail with BadValue when the failpoint is enabled",
        );
        failPoint.off();

        checkCleanupComplete(this.st.s.getDB("config"), this.st.rs0.getPrimary(), this.targetingNs);
    });
});
