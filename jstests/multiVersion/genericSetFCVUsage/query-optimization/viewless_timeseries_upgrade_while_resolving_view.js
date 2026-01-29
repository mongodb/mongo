/**
 * Tests that aggregations return correct results when a timeseries collection is upgraded from
 * viewful to viewless format mid-query. In particular, if the aggregation resolves the timeseries
 * view but the buckets collection is transformed to viewless timeseries before it can be acquired.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// TODO(SERVER-114573): Remove this test once 9.0 becomes lastLTS.
if (lastLTSFCV != "8.0") {
    quit();
}

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const collName = jsTestName();
const testDB = primary.getDB(dbName);
const coll = testDB[collName];

// Create timeseries in viewful format.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: "t"}}));
assert.commandWorked(coll.insertOne({t: ISODate()}));

// Pause aggregation after resolving the view but before acquiring buckets.
const fp = configureFailPoint(primary, "hangAfterAcquiringCollectionCatalog", {collection: collName});

const aggThread = new Thread(
    function (host, dbName, collName) {
        const conn = new Mongo(host);
        const result = conn.getDB(dbName).runCommand({aggregate: collName, pipeline: [], cursor: {}});
        assert.commandWorked(result);
        assert.eq(1, result.cursor.firstBatch.length);
    },
    primary.host,
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

rst.stopSet();
