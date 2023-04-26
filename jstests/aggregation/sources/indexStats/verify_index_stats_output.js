/**
 * Basic test to verify the output of $indexStats.
 *
 * @tags: [
 *   assumes_read_concern_unchanged,
 *   # We are setting the failpoint only on primaries, so we need to disable reads from secondaries,
 *   # where the failpoint is not enabled.
 *   assumes_read_preference_unchanged,
 *   # $indexStats aggregation stage cannot be used with $facet.
 *   do_not_wrap_aggregations_in_facets,
 *   uses_parallel_shell,
 * ]
 */
(function() {
"use strict";
load('jstests/noPassthrough/libs/index_build.js');  // for waitForIndexBuildToStart().
load('jstests/libs/fixture_helpers.js');            // for runCommandOnEachPrimary.
load("jstests/aggregation/extras/utils.js");        // for resultsEq.
load("jstests/libs/fail_point_util.js");            // for configureFailPoint.

const coll = db.index_stats_output;
coll.drop();

let bulk = coll.initializeUnorderedBulkOp();
const nDocs = 100;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, a: i});
}
assert.commandWorked(bulk.execute());

const indexKey = {
    _id: 1,
    a: 1
};
const indexName = "testIndex";

// Verify that in progress index builds report matching 'spec' and 'building: true' in the output of
// $indexStats.

// Enable 'hangAfterStartingIndexBuild' failpoint on each of the primaries. This will make index
// building process infinite.
const failPoints = FixtureHelpers.mapOnEachShardNode({
    db: db.getSiblingDB("admin"),
    func: (db) => configureFailPoint(db, "hangAfterStartingIndexBuild"),
    primaryNodeOnly: true,
});

const join = startParallelShell(() => {
    const indexName = "testIndex";
    const indexKey = {_id: 1, a: 1};
    assert.commandWorked(db.index_stats_output.createIndex(indexKey, {unique: 1, name: indexName}));
});

// Wait for the failpoint to be hit on each of the primaries.
// This ensures that the index build started. We cannot use
// 'IndexBuildTest.waitForIndexBuildToStart()' for it because it checks if any index build operation
// exists. In the sharded cluster it may lead to the situation where only one shard has started
// index build and triggered 'waitForIndexBuildToStart' to return. We want to wait for all shards
// to start index building before proceeding with the test.
// This also ensures that the index was added to the catalog, so that in can be seen by $indexStats
// stage (see SERVER-54172 for details).
failPoints.map((failPoint) => failPoint.wait());

let pausedOutput = coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray();

let allShards = [];
let shardsFound = [];
db.getSiblingDB("config").shards.find().forEach(function(shard) {
    allShards.push(shard._id);
});
const isShardedCluster = !!allShards.length;

for (const indexStats of pausedOutput) {
    assert.hasFields(indexStats, ["building", "spec"]);
    // Each index should report building: true since the index build was paused.
    assert.eq(indexStats["building"], true);
    // Each index should report a spec that matches the parameters passed to createIndex().
    let spec = indexStats["spec"];
    assert.hasFields(spec, ["unique", "name", "key"]);
    assert.eq(spec["unique"], true);
    assert.eq(spec["name"], indexName);
    assert.eq(spec["key"], indexKey);
    // In the sharded case, record the reported shard names and compare them against the
    // names of known shards.
    if (indexStats.hasOwnProperty("shard")) {
        shardsFound.push(indexStats["shard"]);
    } else {
        assert(!isShardedCluster);
    }
}

for (const shard of shardsFound) {
    assert.contains(shard, allShards);
}

// Turn off failpoint on each of the primaries
failPoints.map((failPoint) => failPoint.off());

// Wait until all index building operations stop. It is safe to use
// 'IndexBuildTest.waitForIndexBuildToStop()' here because it ensures that no index building
// operation exists. So in the sharded cluster, this function will return only when all shards
// stopped index building.
IndexBuildTest.waitForIndexBuildToStop(db, coll.getName(), indexName);

join();

// Verify that there is no 'building' field in the $indexStats output for our created index once the
// index build is complete.
let finishedOutput = coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray();
for (const indexStats of finishedOutput) {
    assert(!indexStats.hasOwnProperty("building"), tojson(indexStats));
}
})();
