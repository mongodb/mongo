/**
 * Tests that downgrading FCV below 8.3 fails when v4 2dsphere indexes exist.
 * This test covers both sharded clusters and replica sets.
 *
 * This test:
 * 1. Starts a sharded cluster and replica set with latest version
 * 2. Creates some v4 2dsphere indexes
 * 3. Attempts to downgrade FCV to 8.0
 * 4. Validates that the FCV downgrade fails with CannotDowngrade error
 * 5. Drops the v4 indexes
 * 6. Attempts to downgrade FCV again and validates that it succeeds
 * 7. Tests that v3 2dsphere indexes can be created on 8.3 binary with FCV 8.0
 *
 * TODO SERVER-118561 Remove this test file when 9.0 is last LTS.
 */

import "jstests/multiVersion/libs/verify_versions.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const targetDowngradeVersion = "8.0";

/**
 * Runs the v4 2dsphere index downgrade test against the provided connection.
 * @param {Mongo} conn - The connection to run the test against (mongos or replica set primary)
 * @param {string} testName - The name of the test for logging purposes
 */
function runS2IndexV4DowngradeTest(conn, testName) {
    jsTest.log.info(
        `Starting test: ${testName} - downgrading FCV below 8.3 should fail when v4 2dsphere indexes exist`,
    );

    const testDB = conn.getDB(jsTestName() + "_" + testName.replace(/\s+/g, "_"));
    const coll = testDB.getCollection("geo_coll");

    // Verify we're on latest FCV.
    const adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);

    jsTest.log.info(`[${testName}] Creating v4 2dsphere indexes`);

    // Create a collection and insert some documents with geo data.
    assert.commandWorked(
        coll.insert([
            {_id: 1, location: {type: "Point", coordinates: [40, -70]}},
            {_id: 2, location: {type: "Point", coordinates: [41, -71]}},
            {_id: 3, location: {type: "Point", coordinates: [42, -72]}},
        ]),
    );

    // Create multiple v4 2dsphere indexes.
    assert.commandWorked(
        coll.createIndex({location: "2dsphere"}, {"2dsphereIndexVersion": 4}),
        "Failed to create first v4 2dsphere index",
    );

    // Create another v4 index on a different collection.
    const coll2 = testDB.getCollection("geo_coll2");
    assert.commandWorked(coll2.insert([{_id: 1, geo: {type: "Point", coordinates: [50, -80]}}]));
    assert.commandWorked(
        coll2.createIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 4}),
        "Failed to create second v4 2dsphere index",
    );

    // Verify the indexes were created with version 4.
    const indexes1 = coll.getIndexes();
    const locationIndex = indexes1.find((idx) => idx.name === "location_2dsphere");
    assert.neq(null, locationIndex, "location_2dsphere index not found");
    assert.eq(4, locationIndex["2dsphereIndexVersion"], "Index should have version 4");

    const indexes2 = coll2.getIndexes();
    const geoIndex = indexes2.find((idx) => idx.name === "geo_2dsphere");
    assert.neq(null, geoIndex, "geo_2dsphere index not found");
    assert.eq(4, geoIndex["2dsphereIndexVersion"], "Index should have version 4");

    jsTest.log.info(`[${testName}] Attempting to downgrade FCV to ${targetDowngradeVersion}`);
    const downgradeFCV = binVersionToFCV(targetDowngradeVersion);

    // Attempt to downgrade FCV - this should fail due to v4 indexes.
    jsTest.log.info(`[${testName}] Setting FCV to ${downgradeFCV} - this should fail due to v4 indexes`);
    const fcvResult = adminDB.runCommand({
        setFeatureCompatibilityVersion: downgradeFCV,
        confirm: true,
    });

    // The downgrade should fail with CannotDowngrade error.
    assert.commandFailedWithCode(
        fcvResult,
        ErrorCodes.CannotDowngrade,
        "FCV downgrade should have failed due to v4 indexes",
    );

    jsTest.log.info(`[${testName}] FCV downgrade failed as expected: ${tojson(fcvResult)}`);
    assert(
        fcvResult.errmsg.includes("2dsphere") || fcvResult.errmsg.includes("version 4"),
        "Error message should mention 2dsphere indexes or version 4",
    );

    // Verify FCV is still at latest (downgrade should not have proceeded).
    checkFCV(adminDB, latestFCV);

    // Verify indexes still exist.
    assert.neq(
        null,
        coll.getIndexes().find((idx) => idx.name === "location_2dsphere"),
    );
    assert.neq(
        null,
        coll2.getIndexes().find((idx) => idx.name === "geo_2dsphere"),
    );

    jsTest.log.info(`[${testName}] Dropping v4 2dsphere indexes`);

    // Drop the v4 indexes.
    assert.commandWorked(coll.dropIndex("location_2dsphere"), "Failed to drop location_2dsphere index");
    assert.commandWorked(coll2.dropIndex("geo_2dsphere"), "Failed to drop geo_2dsphere index");

    // Verify indexes are dropped.
    assert.eq(
        null,
        coll.getIndexes().find((idx) => idx.name === "location_2dsphere"),
    );
    assert.eq(
        null,
        coll2.getIndexes().find((idx) => idx.name === "geo_2dsphere"),
    );

    jsTest.log.info(
        `[${testName}] Attempting to downgrade FCV to ${targetDowngradeVersion} again after dropping indexes`,
    );

    // Now attempt to downgrade FCV again - this should succeed.
    const fcvResult2 = adminDB.runCommand({
        setFeatureCompatibilityVersion: downgradeFCV,
        confirm: true,
    });

    // The downgrade should succeed now that v4 indexes are removed.
    assert.commandWorked(fcvResult2, "FCV downgrade should succeed after dropping v4 indexes");

    // Verify FCV was downgraded.
    checkFCV(adminDB, downgradeFCV);
    jsTest.log.info(
        `[${testName}] Test completed successfully: FCV downgrade correctly failed with v4 indexes and succeeded after dropping them`,
    );

    // Test that v3 2dsphere indexes can be created on 8.3 binary with FCV 8.0.
    jsTest.log.info(`[${testName}] Testing v3 2dsphere index creation with FCV ${downgradeFCV}`);

    const coll3 = testDB.getCollection("geo_coll_v3");

    // Insert test documents with geo data.
    assert.commandWorked(
        coll3.insert([
            {_id: 1, location: {type: "Point", coordinates: [40, -70]}},
            {_id: 2, location: {type: "Point", coordinates: [41, -71]}},
            {_id: 3, location: {type: "Point", coordinates: [42, -72]}},
        ]),
    );

    // Create a v3 2dsphere index (new default value) - this should succeed on 8.3 binary with FCV 8.0.
    assert.commandWorked(
        coll3.createIndex({location: "2dsphere"}),
        `Failed to create v3 2dsphere index with FCV ${downgradeFCV}`,
    );

    // Verify the index was created with version 3.
    const indexes3 = coll3.getIndexes();
    const locationIndexV3 = indexes3.find((idx) => idx.name === "location_2dsphere");
    assert.neq(null, locationIndexV3, "location_2dsphere index not found");
    assert.eq(3, locationIndexV3["2dsphereIndexVersion"], "Index should have version 3");

    jsTest.log.info(`[${testName}] Successfully created v3 2dsphere index on 8.3 binary with FCV 8.0`);

    // Verify the index works by running a simple query.
    const result = coll3
        .find({
            location: {
                $near: {
                    $geometry: {type: "Point", coordinates: [40, -70]},
                    $maxDistance: 1000000,
                },
            },
        })
        .toArray();
    assert.gte(result.length, 1, "Query using v3 2dsphere index should return results");

    jsTest.log.info(`[${testName}] v3 2dsphere index creation works on 8.3 binary with FCV 8.0`);

    // Clean up the test database.
    assert.commandWorked(testDB.dropDatabase());
}

// Test with sharded cluster.
(function testShardedCluster() {
    const st = new ShardingTest({shards: 1, mongos: 1});
    try {
        runS2IndexV4DowngradeTest(st.s, "sharded cluster");
    } finally {
        st.stop();
    }
})();

// Test with replica set.
(function testReplicaSet() {
    const rst = new ReplSetTest({nodes: 3});
    rst.startSet();
    rst.initiate();
    try {
        runS2IndexV4DowngradeTest(rst.getPrimary(), "replica set");
    } finally {
        rst.stopSet();
    }
})();
