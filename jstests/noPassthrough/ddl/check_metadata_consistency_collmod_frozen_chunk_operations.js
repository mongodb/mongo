/**
 * Verifies that checkMetadataConsistency does not report a spurious CollectionOptionsMismatch when a
 * lagged secondary reads at a timestamp between the collMod global and local catalog updates.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_replication,
 *   requires_timeseries,
 *   requires_fcv_90,
 *   assumes_balancer_off,
 * ]
 */

// TODO(SERVER-132808): remove this whole test

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});

if (!FeatureFlagUtil.isPresentAndEnabled(st.s, "AuthoritativeShardsDDL")) {
    jsTest.log.info("Skipping test because featureFlagAuthoritativeShardsDDL is disabled");
    st.stop();
    quit();
}

describe("checkMetadataConsistency on a secondary lagged inside a collMod window", function () {
    const dbName = jsTestName();
    const collName = "ts";
    const timeField = "t";
    const metaField = "m";
    const testDB = st.s.getDB(dbName);
    const coll = testDB.getCollection(collName);

    let laggedSecondary = null;

    function checkMetadataConsistency() {
        return testDB
            .checkMetadataConsistency({_checkSecondariesMode: "checkAtSecondaryTimestamp"})
            .toArray();
    }

    before(function () {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        // The sole chunk is placed on shard1, so shard0 keeps a local copy of the collection while
        // owning nothing, which is what leaves it out of the collMod critical section.
        CreateShardedCollectionUtil.shardCollectionWithChunks(
            coll,
            {[metaField]: 1},
            [{min: {meta: MinKey}, max: {meta: MaxKey}, shard: st.shard1.shardName}],
            {timeseries: {timeField: timeField, metaField: metaField, granularity: "seconds"}},
        );

        const trackedNs = getTimeseriesCollForDDLOps(testDB, coll).getFullName();
        const collUUID = st.config.collections.findOne({_id: trackedNs}).uuid;
        assert.eq(
            0,
            st.config.chunks.countDocuments({uuid: collUUID, shard: st.shard0.shardName}),
            "expected the db primary shard to own no chunk",
        );

        assert.eq(0, checkMetadataConsistency().length, "expected a clean starting state");
    });

    it("skips the timeseries options check while chunk operations are frozen", function () {
        laggedSecondary = st.rs0.getSecondaries()[0];
        const fp = configureFailPoint(st.rs0.getPrimary(), "pauseCollModBeforeShardsUpdate");

        const awaitCollMod = startParallelShell(
            funWithArgs(
                function (dbName, collName) {
                    assert.commandWorked(
                        db.getSiblingDB(dbName).runCommand({
                            collMod: collName,
                            timeseries: {bucketMaxSpanSeconds: 7200, bucketRoundingSeconds: 7200},
                        }),
                    );
                },
                dbName,
                collName,
            ),
            st.s.port,
        );

        fp.wait();

        st.rs0.awaitReplication();
        stopServerReplication(laggedSecondary);

        fp.off();
        awaitCollMod();

        const inconsistencies = checkMetadataConsistency();
        assert.eq(0, inconsistencies.length, "found unexpected metadata inconsistencies", {
            inconsistencies,
        });
    });

    after(function () {
        if (laggedSecondary !== null) {
            restartServerReplication(laggedSecondary);
            laggedSecondary = null;
        }
        st.rs0.awaitReplication();

        const inconsistencies = checkMetadataConsistency();
        assert.eq(
            0,
            inconsistencies.length,
            "found unexpected metadata inconsistencies after catching up",
            {inconsistencies},
        );

        assert(coll.drop());
        st.stop();
    });
});
