// Tests the availability and correctness of query settings server status metrics. It does so by
// creating a sharded cluster environment and verifying that the metrics:
// * increment / decrement as expected on inserts / updates / deletions
// * are propagated throughout all mongod / mongos nodes
// * are restored to their previous values on upon restarts
//
// @tags: [
//   featureFlagQuerySettings,
//   requires_fcv_80,
// ]

import "jstests/multiVersion/libs/multi_cluster.js";

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const st = new ShardingTest({name: jsTestName(), shards: 2, mongos: 2, rs: {nodes: 3}});
const db = st.s.getDB("test");

// Creating the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const collName = jsTestName();
assertDropAndRecreateCollection(db, collName);

const primaryQSU = new QuerySettingsUtils(db, collName);
const query = primaryQSU.makeFindQueryInstance({filter: {a: 1}});
const smallerQuerySetting = {
    indexHints: {allowedIndexes: ["a_1"]}
};
const biggerQuerySetting = {
    indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}
};

function restartCluster() {
    st.restartMongoses();
    primaryQSU.db = st.s.getDB(db.getName());  // refresh the db reference so it's possible to later
                                               // call 'clusterParamRefreshSecs.restore()'
    st.stopAllConfigServers({startClean: false}, /* forRestart*/ true);
    st.restartAllConfigServers();
    st.stopAllShards({startClean: false}, /* forRestart */ true);
    st.restartAllShards();
}

// Extends the `jstests/libs/fixture_helpers.js::mapOnEachShardNode()` function to also process
// mongos instances from the current test instance. It runs the `func()` function over all the
// mongod and mongos nodes in the cluster, returning an array according to the defined mapping.
function mapOnEachNode(func) {
    const fromMongos = st._mongos.map((connection) => connection.getDB(db.getName())).map(func);
    const fromShards = FixtureHelpers.mapOnEachShardNode(
        {func, db: st.s.getDB(db.getName()), primaryNodeOnly: false});
    return fromMongos.concat(fromShards);
}

function runTest({expectedQueryShapeConfiguration, assertionFunc}) {
    // Call `assertQueryShapeConfiguration()` on each mongos instance to ensure that the changes
    // introduced by the `setQuerySettings` command have been correctly propagated throughout the
    // whole cluster.
    st.forEachMongos((connection) =>
                         new QuerySettingsUtils(connection.getDB(db.getName()), collName)
                             .assertQueryShapeConfiguration(expectedQueryShapeConfiguration));

    // Ensure that each node emits the exact same query settings metrics, then validate them using
    // the provided `assertionFunc()` assertion.
    const allDataPoints = mapOnEachNode(
        (nodeDB) => new QuerySettingsUtils(nodeDB, collName).getQuerySettingsServerStatus());
    // Check that all the expected nodes have emitted their own set of metrics.
    // Since the test is running in a sharded cluster environment, the number of data points should
    // equal 2 (noMongos) + 2 (noShards) * 3 (noNodesPerReplSet) = 8.
    assert.eq(allDataPoints.length, 8, "Insufficient data points.");
    const representativeDataPoint = allDataPoints[0];
    assert(
        allDataPoints.every((dataPoint) => bsonWoCompare(dataPoint, representativeDataPoint) == 0),
        "Expected all data points to be equal.")
    assertionFunc(representativeDataPoint);
}

// Expect both the count and the size to be zero, as this is a newly created replica set.
runTest({
    expectedQueryShapeConfiguration: [],
    assertionFunc: ({count, size, rejectCount}) => {
        assert.eq(
            count,
            0,
            "`querySettings.count` server status should be 0 if no query settings are present");
        assert.eq(
            size,
            0,
            "`querySettings.size` server status should be 0 if no query settings are present");
        assert.eq(
            rejectCount,
            0,
            "`querySettings.rejectCount` should be zero before any reject settings are applied.");
    }
});

// Keep track of the previous size from now on to verify that it increases/decreases as expected.
let lastSize;

// Insert the bigger entry and verify that the count increases to 1 and that the size is now
// greater than 0.
st.adminCommand({setQuerySettings: query, settings: biggerQuerySetting});
runTest({
    expectedQueryShapeConfiguration:
        [primaryQSU.makeQueryShapeConfiguration(biggerQuerySetting, query)],
    assertionFunc: ({count, size}) => {
        assert.eq(count, 1, "`querySettings.count` server status failed to increase on addition.");
        assert.gt(size, 0, "`querySettings.size` server status failed to increase on addition.");
        lastSize = size;
    }
});

// Replace the query setting with a smaller one and ensure that the size decreases while the
// count remains unchanged.
st.adminCommand({setQuerySettings: query, settings: smallerQuerySetting});
runTest({
    expectedQueryShapeConfiguration:
        [primaryQSU.makeQueryShapeConfiguration(smallerQuerySetting, query)],
    assertionFunc: ({count, size}) => {
        assert.eq(count,
                  1,
                  "`querySettings.count` server status should remain unchanged on replacements.");
        assert.lt(
            size,
            lastSize,
            "`querySettings.size` server status failed to decrease on a smaller replacement.");
        lastSize = size;
    }
});

// Restart the cluster and ensure that the metrics remain unchanged.
restartCluster();
runTest({
    expectedQueryShapeConfiguration:
        [primaryQSU.makeQueryShapeConfiguration(smallerQuerySetting, query)],
    assertionFunc: ({count, size}) => {
        assert.eq(count,
                  1,
                  "`querySettings.count` server status should remain unchanged between restarts.");
        assert.eq(size,
                  lastSize,
                  "`querySettings.size` server status should remain unchanged between restarts.");
        lastSize = size;
    }
});

// Delete the entry and expect the count to be zero and the size to have decreased.
// Since the cluster parameter holds additional data, other than the provided query settings,
// the size is not expected to go back to zero. (ie: `clusterParameterTime`)
st.adminCommand({removeQuerySettings: query});
runTest({
    expectedQueryShapeConfiguration: [],
    assertionFunc: ({count, size}) => {
        assert.eq(count, 0, "`querySettings.count` server status failed to decrease on deletion.");
        assert.lt(
            size, lastSize, "`querySettings.size` server status failed to decrease on deletion.");
    }
});

const rejectQuerySetting = {
    "reject": true
};
const rejectQueryConfig = primaryQSU.makeQueryShapeConfiguration(rejectQuerySetting, query);
// Set reject=true for the test query.
st.adminCommand({setQuerySettings: query, settings: rejectQuerySetting});

// Confirm rejectCount was updated.
runTest({
    expectedQueryShapeConfiguration: [rejectQueryConfig],
    assertionFunc: ({rejectCount}) => {
        assert.eq(
            rejectCount,
            1,
            "`querySettings.rejectCount` should be one after reject has been set for test query.");
    }
});

restartCluster();

// Confirm rejectCount persists across restart.
runTest({
    expectedQueryShapeConfiguration: [rejectQueryConfig],
    assertionFunc: ({rejectCount}) => {
        assert.eq(rejectCount, 1, "`querySettings.rejectCount` should still be one after restart.");
    }
});

// Remove settings.
st.adminCommand({removeQuerySettings: query});

runTest({
    expectedQueryShapeConfiguration: [],
    assertionFunc: ({rejectCount}) => {
        assert.eq(rejectCount,
                  0,
                  "`querySettings.rejectCount` should be zero after settings are removed.");
    }
});

restartCluster();

// Confirm reject is not accidentally restored after restart.
runTest({
    expectedQueryShapeConfiguration: [],
    assertionFunc: ({rejectCount}) => {
        assert.eq(rejectCount, 0, "`querySettings.rejectCount` should be zero after restart.");
    }
});

st.stop();
