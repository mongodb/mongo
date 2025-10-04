/**
 * Tests the aggregation stages marked with 'LiteParsedDocumentSource::AllowedWithApiStrict'
 * flags on sharded collections.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_51,
 *   uses_api_parameters,
 * ]
 */

import {getSingleNodeExplain} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const mongos = st.s0;
const dbName = jsTestName();
const db = mongos.getDB(dbName);

// Test that a $changeStream can be opened with 'apiStrict: true'.
const result = db.runCommand({
    aggregate: 1,
    pipeline: [{$changeStream: {}}],
    cursor: {},
    writeConcern: {w: "majority"},
    apiVersion: "1",
    apiStrict: true,
});
assert.commandWorked(result);

// Tests that sharded time-series collection can be queried (invoking $_internalUnpackBucket stage)
// from an external client with 'apiStrict'.
(function testInternalUnpackBucketAllowance() {
    const collName = "timeseriesColl";
    const timeField = "tm";
    const coll = db[collName];
    assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: timeField}}));
    assert.commandWorked(coll.insert({[timeField]: ISODate("2021-01-01")}));

    // Check querying time-series collection with APIStrict (using $_internalUnpackBucket stage) is
    // allowed before sharding.
    assert.commandWorked(
        db.runCommand({
            find: collName,
            apiVersion: "1",
            apiStrict: true,
        }),
    );
    assert.commandWorked(
        db.runCommand({
            aggregate: collName,
            pipeline: [{$match: {}}],
            cursor: {},
            apiVersion: "1",
            apiStrict: true,
        }),
    );
    const unshardedPlans = [
        getSingleNodeExplain(coll.find().explain()),
        getSingleNodeExplain(coll.explain().aggregate([{$match: {}}])),
    ];
    assert(
        unshardedPlans.every((plan) => plan.stages.map((x) => Object.keys(x)[0]).includes("$_internalUnpackBucket")),
    );

    // Shard the time-series collection.
    const shardKey = {[timeField]: 1};
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(
        mongos.adminCommand({
            shardCollection: `${dbName}.${collName}`,
            key: shardKey,
        }),
    );

    // Check querying time-series collection with APIStrict (using $_internalUnpackBucket stage) is
    // allowed after sharding.
    assert.commandWorked(
        db.runCommand({
            find: collName,
            apiVersion: "1",
            apiStrict: true,
        }),
    );
    assert.commandWorked(
        db.runCommand({
            aggregate: collName,
            pipeline: [{$match: {}}],
            cursor: {},
            apiVersion: "1",
            apiStrict: true,
        }),
    );
    const shardedPlans = [coll.find().explain(), coll.explain().aggregate([{$match: {}}])];
    assert(
        shardedPlans.every((plan) =>
            plan.shards[st.getPrimaryShard(dbName).shardName].stages
                .map((x) => Object.keys(x)[0])
                .includes("$_internalUnpackBucket"),
        ),
    );
})();

st.stop();
