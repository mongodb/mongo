/**
 * Basic test to verify the output of $indexStats.
 *
 * @tags: [assumes_read_concern_unchanged, requires_wiredtiger, do_not_wrap_aggregations_in_facets]
 */
(function() {
"use strict";
load('jstests/noPassthrough/libs/index_build.js');  // for waitForIndexBuildToStart().
load('jstests/libs/fixture_helpers.js');            // for runCommandOnEachPrimary.
load("jstests/aggregation/extras/utils.js");        // for resultsEq.

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
FixtureHelpers.runCommandOnEachPrimary({
    db: db.getSiblingDB("admin"),
    cmdObj: {configureFailPoint: "hangAfterStartingIndexBuild", mode: "alwaysOn"}
});

const join = startParallelShell(() => {
    const indexName = "testIndex";
    const indexKey = {_id: 1, a: 1};
    assert.commandWorked(db.index_stats_output.createIndex(indexKey, {unique: 1, name: indexName}));
});

IndexBuildTest.waitForIndexBuildToStart(db, coll.getName(), indexName);

let pausedOutput = coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray();

let allShards = [];
let shardsFound = [];
db.getSiblingDB("config").shards.find().forEach(function(shard) {
    allShards.push(shard._id);
});

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
    }
}

for (const shard of shardsFound) {
    assert.contains(shard, allShards);
}

FixtureHelpers.runCommandOnEachPrimary({
    db: db.getSiblingDB("admin"),
    cmdObj: {configureFailPoint: "hangAfterStartingIndexBuild", mode: "off"}
});
join();

// Verify that index build has stopped before checking for the 'building' field.
IndexBuildTest.waitForIndexBuildToStop(db, coll.getName(), indexName);

// Verify that there is no 'building' field in the $indexStats output for our created index once the
// index build is complete.
let finishedOutput = coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray();
for (const indexStats of finishedOutput) {
    assert(!indexStats.hasOwnProperty("building"), tojson(indexStats));
}
})();