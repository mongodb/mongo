/**
 * Tests that aggregations on a sharded viewful timeseries collection throw
 * InterruptedDueToTimeseriesUpgradeDowngrade when the collection is upgraded to viewless format
 * mid-query. In the sharded case, the shard cannot internally retry (unlike a replica set), so the
 * error bubbles up to the client.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

// TODO(SERVER-111172): Remove this test once 9.0 becomes lastLTS.
if (lastLTSFCV != "8.0") {
    quit();
}

const st = new ShardingTest({shards: 1});
const dbName = jsTestName();
const collName = jsTestName();
const testDB = st.s.getDB(dbName);
const coll = testDB[collName];

// Create sharded timeseries in viewful format.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: coll.getFullName(),
        key: {m: 1},
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(coll.insertOne({t: ISODate(), m: 1}));

// Pause aggregation on the shard after resolving the view but before acquiring buckets.
const fp = configureFailPoint(st.rs0.getPrimary(), "hangAfterAcquiringCollectionCatalog", {
    collection: getTimeseriesBucketsColl(collName),
});

const aggThread = new Thread(
    function (host, dbName, collName) {
        const conn = new Mongo(host);

        // The first attempt gets interrupted due to the upgrade/downgrade.
        assert.commandFailedWithCode(
            conn.getDB(dbName).runCommand({aggregate: collName, pipeline: [], cursor: {}}),
            ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade,
        );

        // Upon a retry, the aggregation will work correctly.
        const result = conn.getDB(dbName).runCommand({aggregate: collName, pipeline: [], cursor: {}});
        assert.commandWorked(result);
        assert.eq(1, result.cursor.firstBatch.length);
    },
    st.s.host,
    dbName,
    collName,
);

aggThread.start();
fp.wait();

// Upgrade FCV while aggregation is paused - converts viewful to viewless.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

// Release aggregation. We expect non-empty results.
fp.off();
aggThread.join();

st.stop();
