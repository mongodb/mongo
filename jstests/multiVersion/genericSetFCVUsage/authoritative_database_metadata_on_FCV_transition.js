/**
 * Basic test to validate that upgrading and downgrading between an authoritative and
 * non-authoritative database shards work as expected and do not block setFCV.
 *
 * TODO (SERVER-98118): Remove this test.
 *
 * @tags: [
 *   featureFlagShardAuthoritativeDbMetadataCRUD,
 *   featureFlagShardAuthoritativeDbMetadataDDL,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const describeOrSkip = (() => {
    let st;
    try {
        st = new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}});
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        const db = st.s.getDB("admin");
        if (
            FeatureFlagUtil.isPresentAndEnabled(db, "ShardAuthoritativeDbMetadataDDL") ||
            FeatureFlagUtil.isPresentAndEnabled(db, "ShardAuthoritativeDbMetadataCRUD")
        ) {
            jsTest.log.info(
                "Skipping test because ShardAuthoritativeDbMetadata feature flags are already enabled in lastLTS",
            );
            return describe.skip;
        }
    } finally {
        if (st) {
            st.stop();
        }
    }

    return describe;
})();

describeOrSkip("FCV lifecycle for authoritative database metadata", function () {
    const kDbName = "testDb";
    const kAuthoritativeDb = "config";
    const kAuthoritativeColl = "shard.catalog.databases";

    let st, mongos, shard0, shard1;

    function assertAuthoritativeCollectionExists(shard, {shouldExist}) {
        const colls = shard.getDB(kAuthoritativeDb).getCollectionNames();
        if (shouldExist) {
            assert.contains(kAuthoritativeColl, colls, `Authoritative collection should exist on ${shard.shardName}`);
        } else {
            assert.eq(
                -1,
                colls.indexOf(kAuthoritativeColl),
                `Authoritative collection should not exist on ${shard.shardName}`,
            );
        }
    }

    function assertFeatureFlags({enabled}) {
        const db = mongos.getDB(kDbName);
        assert.eq(
            enabled,
            FeatureFlagUtil.isPresentAndEnabled(db, "ShardAuthoritativeDbMetadataDDL"),
            `ShardAuthoritativeDbMetadataDDL flag should be ${enabled}`,
        );
        assert.eq(
            enabled,
            FeatureFlagUtil.isPresentAndEnabled(db, "ShardAuthoritativeDbMetadataCRUD"),
            `ShardAuthoritativeDbMetadataCRUD flag should be ${enabled}`,
        );
    }

    beforeEach(function () {
        st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}});
        mongos = st.s;
        shard0 = st.shard0;
        shard1 = st.shard1;

        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));
    });

    afterEach(function () {
        if (st) {
            st.stop();
        }
    });

    it("should correctly create and remove metadata on FCV upgrade and downgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});

        // The upgrade should succeed, creating the collection on shard0.
        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

        assertFeatureFlags({enabled: true});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});

        // The downgrade should succeed, dropping the collection from shard0.
        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});
    });

    it("should drop pre-existing metadata collection on FCV upgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});

        assert.commandWorked(
            shard0.getDB(kAuthoritativeDb).getCollection(kAuthoritativeColl).insertOne({
                _id: kDbName,
            }),
        );
        st.rs0.awaitLastOpCommitted();

        // Manually create the collection on shard0 to simulate a dirty state.
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});

        // Dry-run should succeed and do not block on a dirty state.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, dryRun: true}),
            "Dry-run upgrade should have succeeded",
        );

        // The upgrade should succeed, dropping the bad collection on shard0 and re-creating it.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            "Actual upgrade should have succeeded",
        );

        assertFeatureFlags({enabled: true});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});
    });

    it("should drop pre-existing metadata collection on a non-primary shard during FCV upgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});

        // Manually create the collection on the a non-primary shard.
        assert.commandWorked(
            shard1.getDB(kAuthoritativeDb).getCollection(kAuthoritativeColl).insertOne({
                _id: kDbName,
            }),
        );
        st.rs1.awaitLastOpCommitted();
        assertAuthoritativeCollectionExists(shard1, {shouldExist: true});

        // The upgrade should succeed, dropping the bad collection on shard1 and creating the
        // correct one on shard0.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            "Actual upgrade should have succeeded",
        );

        assertFeatureFlags({enabled: true});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});
    });

    it("should drop metadata collection from all shards on FCV downgrade", function () {
        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});

        // Manually create the collection on shard1 to simulate a dirty state.
        assert.commandWorked(
            shard1.getDB(kAuthoritativeDb).getCollection(kAuthoritativeColl).insertOne({
                _id: kDbName,
            }),
        );
        st.rs1.awaitLastOpCommitted();
        assertAuthoritativeCollectionExists(shard1, {shouldExist: true});

        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        // Assert the collection is gone from both shards.
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});
    });
});
